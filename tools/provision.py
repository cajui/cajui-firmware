#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial==3.5"]
# ///
"""Enroll devices over local USB. Recovery files contain secrets; never publish them."""

import argparse
from enum import IntEnum
import getpass
import json
import os
from pathlib import Path
import re
import secrets
import stat
import sys
import tempfile
import time


# Storage health reported by HELLO when it is not "ready"; see docs/provisioning.md.
STORAGE_FAILURES = {
    "unmounted",
    "identity",
    "read",
    "corrupt",
    "format",
    "role",
    "device",
    "invalid",
    "write",
}


# Error codes the firmware may send; anything else is reported as UNKNOWN.
DEVICE_ERRORS = {"INVALID", "STORAGE", "FULL", "CONFLICT", "NOT_FOUND", "UNAUTHORIZED", "ADMIN"}
# Recovery file phases in order; progress is recorded and never moves backwards.
PHASES = ("new", "receiver_prepared", "both_prepared", "receiver_active", "configured")
NO_NETWORK = "0" * 16
# Mirrors the receiver's uplink limits (cajui_uplink.h). The MQTT username is also the
# telemetry source_id, so it follows the Central identity syntax.
IDENTITY = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,63}")
HOST = re.compile(r"[A-Za-z0-9.-]{1,64}")


class Enrollment(IntEnum):
    # Mirrors cajui::Enrollment as reported by INFO.
    PREPARED = 1
    ACTIVE = 2
    REVOKED = 3


class ProvisioningError(Exception):
    pass


class DeviceTimeout(ProvisioningError):
    """No CJ1 answer: the device may be asleep, rebooting or on another port."""


def identifier(value, digits=16):
    if (
        not isinstance(value, str)
        or len(value) != digits
        or any(c not in "0123456789abcdef" for c in value)
    ):
        raise ProvisioningError("Invalid identifier or credential encoding")
    return value


class SerialLink:
    def __init__(self, port):
        import serial  # Optional during unit tests; pinned in the script metadata.

        # Open with DTR/RTS low: toggling them drives the board's auto-reset circuit,
        # which would restart the ESP32 (or enter its bootloader) on every connection.
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()

    def close(self):
        self.serial.close()

    def reset(self):
        """Pulse RTS (wired to EN) to restart the board, e.g. to wake a sleeping transmitter."""
        self.serial.rts = True
        time.sleep(0.1)
        self.serial.rts = False

    def request(self, command, timeout=5):
        # Never include a request in an exception: PREPARE carries a secret.
        self.serial.reset_input_buffer()
        self.serial.write(("CJ1 " + command + "\n").encode("ascii"))
        self.serial.flush()
        deadline = time.monotonic() + timeout
        pending = b""
        while time.monotonic() < deadline:
            # A read can time out mid-line while the device is busy; keep the fragment.
            pending += self.serial.readline(512)
            if not pending.endswith(b"\n"):
                continue
            response, pending = pending, b""
            if not response.startswith(b"CJ1 "):
                continue
            fields = response.decode("ascii", errors="replace").strip().split()
            if len(fields) >= 3 and fields[:2] == ["CJ1", "ERR"]:
                code = fields[2] if fields[2] in DEVICE_ERRORS else "UNKNOWN"
                raise ProvisioningError("Device rejected request: " + code)
            if len(fields) >= 3 and fields[:2] == ["CJ1", "OK"] and fields[2] == command.split()[0]:
                return fields[3:]
            raise ProvisioningError("Unexpected device response")
        raise DeviceTimeout("USB response timed out; retain the recovery file and resume")

    def probe(self, command, window):
        """Repeat a read-only command until answered: bytes sent while booting are lost."""
        deadline = time.monotonic() + window
        while True:
            try:
                return self.request(command, timeout=0.3)
            except DeviceTimeout:
                if time.monotonic() >= deadline:
                    raise


def parse_hello(fields):
    # Nine fields since the single-image firmware; eight from older admin-only images.
    if len(fields) == 8:
        fields = [*fields, "admin"]
    if len(fields) != 9 or fields[1] not in {"tx", "rx"} or fields[8] not in {"admin", "run"}:
        raise ProvisioningError("Unexpected device status")
    if fields[2] != "ready":
        reason = fields[2] if fields[2] in STORAGE_FAILURES else "unknown"
        raise ProvisioningError("Device storage is unavailable: " + reason)
    if not fields[6].isascii() or not fields[6].isdigit() or not 0 <= int(fields[6]) <= 128:
        raise ProvisioningError("Invalid queue status")
    return {
        "device": identifier(fields[0]),
        "role": fields[1],
        "network": identifier(fields[3]),
        "receiver": identifier(fields[4]),
        "profile": identifier(fields[5], 4),
        "queued": int(fields[6]),
        "boot": identifier(fields[7], 8),
        "mode": fields[8],
    }


def hello(link):
    return parse_hello(link.request("HELLO"))


# A transmitter sleeps between samples; restarting it opens a short awake window.
WAKE_WINDOW = 8
ADMIN_WINDOW = 15


def wake(link):
    try:
        return hello(link)
    except DeviceTimeout:
        link.reset()
        return parse_hello(link.probe("HELLO", WAKE_WINDOW))


def await_boot(link, previous, mode, window=ADMIN_WINDOW):
    """Wait for a new boot in the given mode; the device restarts after replying."""
    deadline = time.monotonic() + window
    while True:
        try:
            status = parse_hello(link.probe("HELLO", 2))
            if status["boot"] != previous and status["mode"] == mode:
                return status
        except DeviceTimeout:
            pass
        if time.monotonic() >= deadline:
            raise ProvisioningError("Device did not restart in " + mode + " mode")


def ready(link):
    """Return the device in admin mode (radio stopped), switching modes if needed."""
    status = wake(link)
    if status["mode"] == "admin":
        return status
    link.request("ADMIN " + status["device"])
    return await_boot(link, status["boot"], "admin")


def start_pairing(link):
    """Restart a transmitter into radio pairing (docs/radio-pairing.md); no admin mode needed."""
    status = wake(link)
    if status["role"] != "tx":
        raise ProvisioningError("Radio pairing starts on a transmitter")
    link.request("PAIR " + status["device"])
    return {"pairing": True, "node": status["device"], "window_seconds": 120}


def release(link, device):
    """Leave admin mode: the device restarts its radio application if enrolled."""
    link.request("REBOOT " + device)


def write_durably(fd, transaction):
    with os.fdopen(fd, "wb") as output:
        output.write((json.dumps(transaction, sort_keys=True) + "\n").encode())
        output.flush()
        os.fsync(output.fileno())


def sync_directory(path):
    directory = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def create(path, transaction):
    """Write a new owner-only recovery file; never overwrite an existing one."""
    path = Path(path)
    write_durably(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), transaction)
    sync_directory(path)


def save(path, transaction):
    """Replace an existing recovery file atomically."""
    path = Path(path)
    # Atomic replacement prevents a host interruption from truncating recovery state.
    fd, temporary = tempfile.mkstemp(prefix=".cajui-", dir=path.parent)
    try:
        write_durably(fd, transaction)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    sync_directory(path)


def random_id():
    return f"{secrets.randbelow((1 << 64) - 1) + 1:016x}"  # Nonzero, 64 bits.


def advance(path, transaction, phase):
    if PHASES.index(phase) > PHASES.index(transaction["phase"]):
        transaction["phase"] = phase
        save(path, transaction)


def load(path):
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(fd, "r") as source:
        info = os.fstat(source.fileno())
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_mode & 0o077
            or info.st_uid != os.getuid()
            or info.st_size > 4096
        ):
            raise ProvisioningError("Recovery file must be an owner-only regular file (mode 0600)")
        try:
            transaction = json.load(source)
        except (ValueError, TypeError):
            raise ProvisioningError("Invalid recovery file") from None
    required = {"version", "network", "receiver", "node", "generation", "key", "profile", "phase"}
    if (
        not isinstance(transaction, dict)
        or set(transaction) != required
        or transaction["version"] != 1
    ):
        raise ProvisioningError("Unsupported recovery file")
    for name in ("network", "receiver", "node", "generation"):
        if int(identifier(transaction[name]), 16) == 0:
            raise ProvisioningError("Recovery identifiers must not be zero")
    if not int(identifier(transaction["key"], 32), 16) or transaction["profile"] != "0001":
        raise ProvisioningError("Invalid credential or radio profile")
    if transaction["phase"] not in PHASES:
        raise ProvisioningError("Invalid enrollment phase")
    return transaction


def check_devices(tx, rx, transaction=None):
    transmitter, receiver = ready(tx), ready(rx)
    if (
        transmitter["role"] != "tx"
        or receiver["role"] != "rx"
        or transmitter["device"] == receiver["device"]
    ):
        raise ProvisioningError("Expected two distinct devices with tx/rx administration firmware")
    if transaction:
        if (
            transmitter["device"] != transaction["node"]
            or receiver["device"] != transaction["receiver"]
        ):
            raise ProvisioningError("Device identity changed; select the intended USB ports")
        for device in (transmitter, receiver):
            if device["network"] != NO_NETWORK and any(
                device[field] != transaction[field] for field in ("network", "receiver", "profile")
            ):
                raise ProvisioningError("Device belongs to a different network/profile")
    return transmitter, receiver


def get_info(link, device, transaction):
    values = link.request(f"INFO {device} {transaction['node']} {transaction['generation']}")
    if len(values) != 2 or values[0] not in {str(state.value) for state in Enrollment}:
        raise ProvisioningError("Invalid enrollment status")
    return Enrollment(int(values[0])), int(identifier(values[1]), 16)


def enroll(tx, rx, path, resume=False):
    if resume:
        transaction = load(path)
        check_devices(tx, rx, transaction)
        # If a device that was already prepared lost its state, do not restore its old key.
        if transaction["phase"] != "new":
            get_info(rx, transaction["receiver"], transaction)
        if transaction["phase"] in {"both_prepared", "receiver_active", "configured"}:
            get_info(tx, transaction["node"], transaction)
    else:
        transmitter, receiver = check_devices(tx, rx)
        network = receiver["network"] if receiver["network"] != NO_NETWORK else random_id()
        transaction = {
            "version": 1,
            "network": network,
            "receiver": receiver["device"],
            "node": transmitter["device"],
            "generation": random_id(),
            "key": secrets.token_hex(16),
            "profile": "0001",
            "phase": "new",
        }
        check_devices(tx, rx, transaction)
        create(path, transaction)  # Persist the secret BEFORE mutating either device.
    for link, device, phase in (
        (rx, transaction["receiver"], "receiver_prepared"),
        (tx, transaction["node"], "both_prepared"),
    ):
        link.request(
            f"PREPARE {device} {transaction['network']} {transaction['receiver']} "
            f"{transaction['node']} {transaction['generation']} {transaction['key']} {transaction['profile']}"
        )
        advance(path, transaction, phase)
    for link, device, phase in (
        (rx, transaction["receiver"], "receiver_active"),
        (tx, transaction["node"], "configured"),
    ):
        link.request(f"ACTIVATE {device} {transaction['node']} {transaction['generation']}")
        if get_info(link, device, transaction)[0] != Enrollment.ACTIVE:
            raise ProvisioningError("Device did not activate the enrollment")
        advance(path, transaction, phase)
    release(rx, transaction["receiver"])
    release(tx, transaction["node"])
    return {
        "node": transaction["node"],
        "receiver": transaction["receiver"],
        "network": transaction["network"],
        "configured": True,
        "radio_validated": False,
    }


def verify_restart(tx, rx, path):
    transaction = load(path)
    boots_before = [device["boot"] for device in check_devices(tx, rx, transaction)]
    for link, device in ((tx, transaction["node"]), (rx, transaction["receiver"])):
        if get_info(link, device, transaction)[0] != Enrollment.ACTIVE:
            raise ProvisioningError("Both devices must have active enrollment")
    transmitter = transaction["node"]  # A transmitter's device ID is its node ID.
    command = f"RESERVE {transmitter} {transaction['node']} {transaction['generation']}"
    before = int(identifier(tx.request(command)[0]), 16)
    # ADMIN restarts like REBOOT but keeps the radio stopped for the checks below.
    tx.request("ADMIN " + transmitter)
    rx.request("ADMIN " + transaction["receiver"])
    for link, old in zip((tx, rx), boots_before):
        await_boot(link, old, "admin")
    check_devices(tx, rx, transaction)
    for link, device in ((tx, transaction["node"]), (rx, transaction["receiver"])):
        if get_info(link, device, transaction)[0] != Enrollment.ACTIVE:
            raise ProvisioningError("Enrollment did not survive restart")
    after = int(identifier(tx.request(command)[0]), 16)
    if after <= before:
        raise ProvisioningError("Counter did not advance across restart")
    release(tx, transaction["node"])
    release(rx, transaction["receiver"])
    return {
        "configured": True,
        "restart_verified": True,
        "counter_before": before,
        "counter_after": after,
        "radio_validated": False,
    }


def revoke(rx, transaction):
    device = ready(rx)
    if device["device"] != transaction["receiver"] or device["role"] != "rx":
        raise ProvisioningError("Wrong receiver")
    rx.request(f"REVOKE {device['device']} {transaction['node']} {transaction['generation']}")
    release(rx, device["device"])
    return {"revoked": True, "node": transaction["node"]}


def read_secret(path, prompt):
    """Read a secret from a file (first line) or an interactive prompt without echo."""
    if path is None:
        return getpass.getpass(prompt)
    with open(path, encoding="utf-8") as source:
        return source.readline().rstrip("\r\n")


def uplink_fields(ssid, wifi_password, host, port, username, password):
    """Validate locally so a device never receives a partial or oversized setting."""

    def size(value):
        return len(value.encode("utf-8"))

    if not 1 <= size(ssid) <= 32 or "\0" in ssid:
        raise ProvisioningError("Wi-Fi SSID must have 1-32 bytes")
    if not 8 <= size(wifi_password) <= 64 or "\0" in wifi_password:
        raise ProvisioningError(
            "Wi-Fi password must have 8-64 bytes; open networks are unsupported"
        )
    if not HOST.fullmatch(host):
        raise ProvisioningError("Broker host must be an IPv4 address or host name")
    if not 1 <= port <= 65535:
        raise ProvisioningError("Broker port must be 1-65535")
    if not IDENTITY.fullmatch(username):
        raise ProvisioningError("MQTT username must be a Central source_id")
    if not 1 <= size(password) <= 64 or "\0" in password:
        raise ProvisioningError("MQTT password must have 1-64 bytes")
    return (
        ("ssid", ssid),
        ("wifipass", wifi_password),
        ("host", host),
        ("port", str(port)),
        ("user", username),
        ("pass", password),
    )


def uplink_info(link, device):
    values = link.request("UPLINKINFO " + device)
    if values == ["0"]:
        return {"configured": False}
    if len(values) != 4 or values[0] != "1" or not values[2].isdigit():
        raise ProvisioningError("Invalid uplink status")
    return {"configured": True, "host": values[1], "port": int(values[2]), "username": values[3]}


def configure_uplink(rx, fields):
    """Stage every field, then save atomically. The device never echoes a secret."""
    device = ready(rx)
    if device["role"] != "rx":
        raise ProvisioningError("Uplink settings require the receiver administration firmware")
    for name, value in fields:
        rx.request(f"UPLINKSET {device['device']} {name} {value.encode('utf-8').hex()}")
    rx.request("UPLINKSAVE " + device["device"])
    stored = uplink_info(rx, device["device"])
    expected = dict(fields)
    if (
        not stored["configured"]
        or stored["host"] != expected["host"]
        or stored["port"] != int(expected["port"])
        or stored["username"] != expected["user"]
    ):
        raise ProvisioningError("Receiver did not store the requested uplink settings")
    release(rx, device["device"])
    return {**stored, "receiver": device["device"], "forwarding_validated": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    status = sub.add_parser("status")
    status.add_argument("--port", required=True)
    for action in ("enroll", "resume", "verify-restart"):
        cmd = sub.add_parser(action)
        cmd.add_argument("--transmitter", required=True)
        cmd.add_argument("--receiver", required=True)
        cmd.add_argument(
            "--state",
            type=Path,
            required=True,
            help="Private recovery file, outside version control",
        )
    revoke_command = sub.add_parser("revoke")
    revoke_command.add_argument("--receiver", required=True)
    revoke_command.add_argument("--state", type=Path, required=True)
    uplink = sub.add_parser("uplink", help="Store Wi-Fi and MQTT forwarding settings")
    uplink.add_argument("--receiver", required=True)
    uplink.add_argument("--ssid", required=True)
    uplink.add_argument("--host", required=True, help="Broker IPv4 address or host name")
    uplink.add_argument("--port", type=int, default=1883)
    uplink.add_argument("--username", required=True, help="MQTT user, also the source_id")
    uplink.add_argument("--wifi-password-file", type=Path, help="Prompts when omitted")
    uplink.add_argument("--mqtt-password-file", type=Path, help="Prompts when omitted")
    uplink_status = sub.add_parser("uplink-status")
    uplink_status.add_argument("--receiver", required=True)
    pair = sub.add_parser("pair", help="Start radio pairing on a transmitter")
    pair.add_argument("--transmitter", required=True)
    args = parser.parse_args()
    links = []
    try:

        def connect(port):
            link = SerialLink(port)
            links.append(link)
            return link

        if args.action == "status":
            output = wake(connect(args.port))  # Read-only: reports the mode, never switches.
        elif args.action == "revoke":
            transaction = load(args.state)
            output = revoke(connect(args.receiver), transaction)
        elif args.action == "uplink":
            fields = uplink_fields(
                args.ssid,
                read_secret(args.wifi_password_file, "Wi-Fi password: "),
                args.host,
                args.port,
                args.username,
                read_secret(args.mqtt_password_file, "MQTT password: "),
            )
            output = configure_uplink(connect(args.receiver), fields)
        elif args.action == "pair":
            output = start_pairing(connect(args.transmitter))
        elif args.action == "uplink-status":
            link = connect(args.receiver)
            output = uplink_info(link, wake(link)["device"])
        else:
            tx, rx = connect(args.transmitter), connect(args.receiver)
            if args.action == "verify-restart":
                output = verify_restart(tx, rx, args.state)
            else:
                output = enroll(tx, rx, args.state, resume=args.action == "resume")
        print(json.dumps(output, sort_keys=True))
    except (ProvisioningError, OSError) as error:
        print(f"Provisioning failed: {error}", file=sys.stderr)
        return 1
    finally:
        for link in links:
            link.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
