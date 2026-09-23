import contextlib
import importlib.util
import io
import itertools
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "provision", Path(__file__).resolve().parents[1] / "tools/provision.py"
)
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)


class Device:
    def __init__(self, role, identity, events):
        self.role, self.identity, self.events = role, identity, events
        self.network, self.receiver, self.profile = "0" * 16, "0" * 16, "0000"
        self.enrollment = None
        self.state, self.counter = 0, 0
        self.fail = None
        self.boot = 1
        self.reboots = True
        self.closed = False
        self.staged, self.uplink, self.info = {}, {}, None

    def close(self):
        self.closed = True

    def request(self, command):
        words = command.split()
        verb = words[0]
        self.events.append((self.role, verb))
        if self.fail == verb:
            self.fail = None
            raise provision.ProvisioningError("Simulated disconnect")
        if verb == "HELLO":
            return [
                self.identity,
                self.role,
                "ready",
                self.network,
                self.receiver,
                self.profile,
                "0",
                f"{self.boot:08x}",
            ]
        if words[1] != self.identity:
            raise provision.ProvisioningError("Wrong device identity")
        if verb == "PREPARE":
            proposed = words[2:]
            if self.enrollment == proposed:
                return []
            if self.enrollment is not None:
                raise provision.ProvisioningError("Different transaction")
            self.enrollment = proposed
            self.network, self.receiver, self.profile = words[2], words[3], words[7]
            self.state = 1
        elif verb == "INFO":
            if self.enrollment is None:
                raise provision.ProvisioningError("Device rejected request: NOT_FOUND")
            return [str(self.state), f"{self.counter:016x}"]
        elif verb == "ACTIVATE":
            self.state = 2
        elif verb == "RESERVE":
            self.counter += 1
            return [f"{self.counter:016x}"]
        elif verb == "REVOKE":
            if self.enrollment is None:
                raise provision.ProvisioningError("Device rejected request: NOT_FOUND")
            self.state = 3
        elif verb == "REBOOT":
            self.boot += int(self.reboots)  # Durable state survives; C++ tests cover storage.
        elif verb == "UPLINKSET":
            self.staged[words[2]] = bytes.fromhex(words[3]).decode("utf-8")
        elif verb == "UPLINKSAVE":
            self.uplink = dict(self.staged)
            self.staged = {}
        elif verb == "UPLINKINFO":
            if self.info is not None:
                return self.info
            if not self.uplink:
                return ["0"]
            return ["1", self.uplink["host"], self.uplink["port"], self.uplink["user"]]
        else:
            raise provision.ProvisioningError("Unknown command")
        return []


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name) / "recovery.json"
        self.events = []
        self.tx = Device("tx", "0000000000000002", self.events)
        self.rx = Device("rx", "0000000000000001", self.events)

    def tearDown(self):
        self.temporary.cleanup()

    def test_enrollment_orders_steps_and_hides_credentials(self):
        output = provision.enroll(self.tx, self.rx, self.path)
        self.assertTrue(output["configured"])
        self.assertFalse(output["radio_validated"])
        transaction = provision.load(self.path)
        self.assertEqual(0o600, self.path.stat().st_mode & 0o777)
        self.assertNotIn(transaction["key"], json.dumps(output))
        mutations = [item for item in self.events if item[1] in {"PREPARE", "ACTIVATE"}]
        self.assertEqual(
            [("rx", "PREPARE"), ("tx", "PREPARE"), ("rx", "ACTIVATE"), ("tx", "ACTIVATE")],
            mutations,
        )
        self.assertEqual("configured", transaction["phase"])

    def test_each_interruption_resumes_with_the_original_credentials(self):
        # Counter continuity is firmware state, covered by the C++ provisioning tests.
        for role, verb in (
            ("rx", "PREPARE"),
            ("tx", "PREPARE"),
            ("rx", "ACTIVATE"),
            ("tx", "ACTIVATE"),
        ):
            with self.subTest(role=role, verb=verb), tempfile.TemporaryDirectory() as folder:
                tx, rx = Device("tx", self.tx.identity, []), Device("rx", self.rx.identity, [])
                path = Path(folder) / "recovery.json"
                (tx if role == "tx" else rx).fail = verb
                with self.assertRaises(provision.ProvisioningError):
                    provision.enroll(tx, rx, path)
                initial = provision.load(path)
                provision.enroll(tx, rx, path, resume=True)
                resumed = provision.load(path)
                self.assertEqual("configured", resumed["phase"])
                for field in ("network", "generation", "key"):
                    self.assertEqual(initial[field], resumed[field])
                # The fake rejects a PREPARE that differs from the enrollment it holds.
                self.assertEqual(initial["generation"], tx.enrollment[3])

    def test_recovery_file_exists_before_first_device_write(self):
        original = self.rx.request

        def inspect(command):
            if command.startswith("PREPARE"):
                self.assertEqual("new", provision.load(self.path)["phase"])
            return original(command)

        self.rx.request = inspect
        provision.enroll(self.tx, self.rx, self.path)

    def test_wrong_ports_or_network_are_rejected_before_mutation(self):
        with self.assertRaises(provision.ProvisioningError):
            provision.enroll(self.rx, self.tx, self.path)
        self.assertFalse(self.path.exists())
        self.tx.network = "000000000000ffff"
        with self.assertRaises(provision.ProvisioningError):
            provision.enroll(self.tx, self.rx, self.path)
        self.assertIsNone(self.rx.enrollment)

    def test_resume_rejects_erased_previously_configured_device(self):
        provision.enroll(self.tx, self.rx, self.path)
        self.tx.enrollment = None
        self.tx.network, self.tx.receiver, self.tx.profile = "0" * 16, "0" * 16, "0000"
        with self.assertRaises(provision.ProvisioningError):
            provision.enroll(self.tx, self.rx, self.path, resume=True)
        self.assertIsNone(self.tx.enrollment)

    def test_file_permissions_schema_and_symlink(self):
        provision.enroll(self.tx, self.rx, self.path)
        os.chmod(self.path, 0o644)
        with self.assertRaises(provision.ProvisioningError):
            provision.load(self.path)
        os.chmod(self.path, 0o600)
        link = self.path.with_name("link.json")
        link.symlink_to(self.path)
        with self.assertRaises(OSError):
            provision.load(link)
        transaction = provision.load(self.path)
        transaction["unexpected"] = True
        provision.save(self.path, transaction)
        with self.assertRaises(provision.ProvisioningError):
            provision.load(self.path)

    def test_create_never_overwrites_existing_recovery(self):
        provision.enroll(self.tx, self.rx, self.path)
        before = self.path.read_bytes()
        with self.assertRaises(FileExistsError):
            provision.enroll(self.tx, self.rx, self.path)
        self.assertEqual(before, self.path.read_bytes())

    def test_verify_restart_burns_counters_but_does_not_claim_radio(self):
        provision.enroll(self.tx, self.rx, self.path)
        result = provision.verify_restart(self.tx, self.rx, self.path)
        self.assertTrue(result["restart_verified"])
        self.assertEqual(1, result["counter_before"])
        self.assertEqual(2, result["counter_after"])
        self.assertFalse(result["radio_validated"])

    def test_counter_reset_on_reboot_fails_verification(self):
        provision.enroll(self.tx, self.rx, self.path)
        original = self.tx.request

        def resetting(command):
            if command.startswith("REBOOT"):
                self.tx.counter = 0
            return original(command)

        self.tx.request = resetting
        with self.assertRaises(provision.ProvisioningError):
            provision.verify_restart(self.tx, self.rx, self.path)

    def test_revoke_targets_only_the_enrolled_receiver(self):
        provision.enroll(self.tx, self.rx, self.path)
        transaction = provision.load(self.path)
        with self.assertRaises(provision.ProvisioningError):
            provision.revoke(self.tx, transaction)
        self.assertEqual(provision.Enrollment.ACTIVE, self.tx.state)
        self.assertEqual(
            {"revoked": True, "node": self.tx.identity}, provision.revoke(self.rx, transaction)
        )
        self.assertEqual(provision.Enrollment.REVOKED, self.rx.state)
        self.assertEqual(provision.Enrollment.ACTIVE, self.tx.state)

    def test_malformed_identifiers_and_recovery_file(self):
        for value in (None, 123, "abc", "g" * 16, "F" * 16):
            with self.assertRaises(provision.ProvisioningError):
                provision.identifier(value)
        provision.enroll(self.tx, self.rx, self.path)
        transaction = provision.load(self.path)
        for field, bad in (
            ("version", 9),
            ("generation", "0" * 16),
            ("key", "0" * 32),
            ("profile", "0002"),
            ("phase", "invalid"),
        ):
            changed = dict(transaction)
            changed[field] = bad
            provision.save(self.path, changed)
            with self.assertRaises(provision.ProvisioningError):
                provision.load(self.path)


class FailureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name) / "recovery.json"
        self.tx = Device("tx", "0000000000000002", [])
        self.rx = Device("rx", "0000000000000001", [])

    def tearDown(self):
        self.temporary.cleanup()

    def test_ready_retries_until_the_deadline(self):
        self.tx.fail = "HELLO"
        with patch.object(provision.time, "sleep"):
            self.assertEqual("tx", provision.ready(self.tx)["role"])

        class Down:
            def request(self, command):
                raise provision.ProvisioningError("Simulated disconnect")

        with (
            patch.object(provision.time, "sleep"),
            patch.object(provision.time, "monotonic", side_effect=itertools.count(0, 5)),
        ):
            with self.assertRaises(provision.ProvisioningError):
                provision.ready(Down())

    def test_restart_without_a_new_boot_fails(self):
        provision.enroll(self.tx, self.rx, self.path)
        self.rx.reboots = False
        with (
            patch.object(provision.time, "sleep"),
            patch.object(provision.time, "monotonic", side_effect=itertools.count(0, 5)),
        ):
            with self.assertRaisesRegex(provision.ProvisioningError, "new boot"):
                provision.verify_restart(self.tx, self.rx, self.path)

    def test_changed_identity_or_network_is_rejected_on_resume(self):
        provision.enroll(self.tx, self.rx, self.path)
        transaction = provision.load(self.path)
        other = Device("tx", "0000000000000003", [])
        with self.assertRaisesRegex(provision.ProvisioningError, "identity"):
            provision.check_devices(other, self.rx, transaction)
        self.rx.network = "000000000000beef"
        with self.assertRaisesRegex(provision.ProvisioningError, "network"):
            provision.check_devices(self.tx, self.rx, transaction)

    def test_invalid_device_answers_are_rejected(self):
        provision.enroll(self.tx, self.rx, self.path)
        transaction = provision.load(self.path)

        class Reply:
            def __init__(self, values):
                self.values = values

            def request(self, command):
                return self.values

        with self.assertRaises(provision.ProvisioningError):
            provision.get_info(Reply(["4", "0" * 16]), self.tx.identity, transaction)
        self.tx.enrollment, self.tx.state = None, 0
        activate = self.tx.request

        def stays_prepared(command):
            result = activate(command)
            if command.startswith("ACTIVATE"):
                self.tx.state = 1
            return result

        self.tx.request = stays_prepared
        os.unlink(self.path)
        self.tx.network, self.tx.receiver, self.tx.profile = "0" * 16, "0" * 16, "0000"
        self.rx.enrollment, self.rx.state = None, 0
        self.rx.network, self.rx.receiver, self.rx.profile = "0" * 16, "0" * 16, "0000"
        with self.assertRaisesRegex(provision.ProvisioningError, "did not activate"):
            provision.enroll(self.tx, self.rx, self.path)

    def test_failed_replace_preserves_recovery_and_removes_temporary_secret(self):
        provision.enroll(self.tx, self.rx, self.path)
        original = self.path.read_bytes()
        transaction = provision.load(self.path)
        transaction["phase"] = "new"
        with patch.object(provision.os, "replace", side_effect=OSError("Simulated I/O failure")):
            with self.assertRaises(OSError):
                provision.save(self.path, transaction)
        self.assertEqual(original, self.path.read_bytes())
        self.assertEqual([self.path], list(self.path.parent.iterdir()))

    def test_restart_requires_active_enrollment_before_any_reservation(self):
        provision.enroll(self.tx, self.rx, self.path)
        self.tx.state = provision.Enrollment.PREPARED
        with self.assertRaisesRegex(provision.ProvisioningError, "active enrollment"):
            provision.verify_restart(self.tx, self.rx, self.path)
        self.assertEqual(0, self.tx.counter)
        self.assertEqual(1, self.tx.boot)
        self.assertEqual(1, self.rx.boot)

    def test_restart_rejects_enrollment_lost_after_reboot(self):
        provision.enroll(self.tx, self.rx, self.path)
        original = self.rx.request

        def lost_enrollment(command):
            response = original(command)
            if command.startswith("REBOOT"):
                self.rx.state = provision.Enrollment.PREPARED
            return response

        self.rx.request = lost_enrollment
        with self.assertRaisesRegex(provision.ProvisioningError, "survive restart"):
            provision.verify_restart(self.tx, self.rx, self.path)
        self.assertEqual(1, self.tx.counter)  # No second reservation after lost enrollment.

    def test_unreadable_recovery_file_is_rejected(self):
        fd = os.open(self.path, os.O_WRONLY | os.O_CREAT, 0o600)
        with os.fdopen(fd, "w") as output:
            output.write("{not json")
        with self.assertRaisesRegex(provision.ProvisioningError, "Invalid recovery file"):
            provision.load(self.path)


class CommandLineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name) / "recovery.json"
        self.devices = {
            "tx-port": Device("tx", "0000000000000002", []),
            "rx-port": Device("rx", "0000000000000001", []),
        }

    def tearDown(self):
        self.temporary.cleanup()

    def run_cli(self, *arguments):
        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            patch.object(provision, "SerialLink", side_effect=self.devices.__getitem__),
            patch.object(sys, "argv", ["provision.py", *arguments]),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            code = provision.main()
        return code, stdout.getvalue(), stderr.getvalue()

    def test_enroll_revoke_and_status_print_json_without_the_key(self):
        pair = ["--transmitter", "tx-port", "--receiver", "rx-port", "--state", str(self.path)]
        code, output, _ = self.run_cli("enroll", *pair)
        self.assertEqual(0, code)
        self.assertTrue(json.loads(output)["configured"])
        self.assertNotIn(provision.load(self.path)["key"], output)
        self.assertEqual(0, self.run_cli("verify-restart", *pair)[0])
        self.assertEqual(0, self.run_cli("resume", *pair)[0])
        code, output, _ = self.run_cli("revoke", "--receiver", "rx-port", "--state", str(self.path))
        self.assertEqual({"revoked": True, "node": "0000000000000002"}, json.loads(output))
        code, output, _ = self.run_cli("status", "--port", "tx-port")
        self.assertEqual("tx", json.loads(output)["role"])
        self.assertTrue(all(device.closed for device in self.devices.values()))

    def test_failures_exit_nonzero_and_still_close_ports(self):
        code, output, error = self.run_cli(
            "resume", "--transmitter", "tx-port", "--receiver", "rx-port", "--state", str(self.path)
        )
        self.assertEqual(1, code)
        self.assertEqual("", output)
        self.assertIn("Provisioning failed", error)
        self.assertTrue(all(device.closed for device in self.devices.values()))


class FakeSerial:
    def __init__(self, lines):
        self.lines = list(lines)
        self.sent = []

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.sent.append(data)

    def flush(self):
        pass

    def readline(self, limit):
        return self.lines.pop(0) if self.lines else b""


class TransportTests(unittest.TestCase):
    def test_port_opens_with_reset_lines_low(self):
        events = []

        class Port:
            def __init__(self, **settings):
                events.append(("created", settings["port"]))

            def __setattr__(self, name, value):
                events.append((name, value))

            def open(self):
                events.append(("open",))

            def close(self):
                events.append(("close",))

        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=Port)}):
            link = provision.SerialLink("port")
            link.close()
        self.assertEqual(
            [
                ("created", None),
                ("dtr", False),
                ("rts", False),
                ("port", "port"),
                ("open",),
                ("close",),
            ],
            events,
        )

    def link(self, lines):
        link = provision.SerialLink.__new__(provision.SerialLink)
        link.serial = FakeSerial(lines)
        return link

    def test_boot_noise_is_ignored_and_command_is_framed(self):
        link = self.link([b"boot output\n", b"CJ1 OK ACTIVATE\n"])
        self.assertEqual([], link.request("ACTIVATE device node generation"))
        self.assertEqual(b"CJ1 ACTIVATE device node generation\n", link.serial.sent[0])

    def test_device_errors_never_echo_a_secret(self):
        secret = "0123456789abcdef0123456789abcdef"
        link = self.link([("CJ1 ERR " + secret + "\n").encode()])
        with self.assertRaises(provision.ProvisioningError) as caught:
            link.request("PREPARE " + secret)
        self.assertNotIn(secret, str(caught.exception))

    def test_timeout_and_wrong_response_type(self):
        link = self.link([])
        with patch.object(provision.time, "monotonic", side_effect=[0, 1, 6]):
            with self.assertRaises(provision.ProvisioningError):
                link.request("HELLO")
        with self.assertRaises(provision.ProvisioningError):
            self.link([b"CJ1 OK PREPARE\n"]).request("HELLO")

    def test_malformed_hello_is_rejected(self):
        class Reply:
            def request(self, command):
                return ["0" * 16, "tx", "ready", "0" * 16, "0" * 16, "0001", "invalid", "00000001"]

        with self.assertRaises(provision.ProvisioningError):
            provision.hello(Reply())

    def test_invalid_hello_shape_role_and_queue_are_rejected(self):
        valid = ["0" * 16, "tx", "ready", "0" * 16, "0" * 16, "0001", "0", "00000001"]
        replies = [valid[:-1], valid[:1] + ["unknown"] + valid[2:]]
        for count in ("-1", "129", "١", ""):
            replies.append(valid[:6] + [count] + valid[7:])
        for fields in replies:
            with self.subTest(fields=fields):
                reply = types.SimpleNamespace(request=lambda _: fields)
                with self.assertRaises(provision.ProvisioningError):
                    provision.hello(reply)

    def test_storage_failure_reason_is_reported(self):
        class Reply:
            def __init__(self, health):
                self.health = health

            def request(self, command):
                return ["0" * 16, "rx", self.health, "0" * 16, "0" * 16, "0001", "0", "00000001"]

        for health, reason in (("role", "role"), ("bogus", "unknown")):
            with self.assertRaises(provision.ProvisioningError) as caught:
                provision.hello(Reply(health))
            self.assertEqual("Device storage is unavailable: " + reason, str(caught.exception))


if __name__ == "__main__":
    unittest.main()


UPLINK = ("Bench net", "wifi secret", "192.168.1.20", 1883, "receiver-1", "mqtt secret")


class UplinkTests(unittest.TestCase):
    def setUp(self):
        self.events = []
        self.rx = Device("rx", "0000000000000001", self.events)

    def test_settings_are_hex_encoded_saved_and_confirmed_without_secrets(self):
        fields = provision.uplink_fields(*UPLINK)
        output = provision.configure_uplink(self.rx, fields)
        self.assertEqual(
            {
                "configured": True,
                "host": "192.168.1.20",
                "port": 1883,
                "username": "receiver-1",
                "receiver": "0000000000000001",
                "forwarding_validated": False,
            },
            output,
        )
        self.assertEqual("wifi secret", self.rx.uplink["wifipass"])
        self.assertEqual(
            ["HELLO"] + ["UPLINKSET"] * 6 + ["UPLINKSAVE", "UPLINKINFO"],
            [verb for _, verb in self.events],
        )
        self.assertNotIn("secret", json.dumps(output))

    def test_stored_settings_must_match_the_request(self):
        self.rx.info = ["1", "10.0.0.1", "1883", "receiver-1"]
        with self.assertRaisesRegex(provision.ProvisioningError, "did not store"):
            provision.configure_uplink(self.rx, provision.uplink_fields(*UPLINK))
        self.rx.info = ["0"]
        with self.assertRaisesRegex(provision.ProvisioningError, "did not store"):
            provision.configure_uplink(self.rx, provision.uplink_fields(*UPLINK))

    def test_transmitter_is_rejected_before_any_setting(self):
        tx = Device("tx", "0000000000000002", self.events)
        with self.assertRaisesRegex(provision.ProvisioningError, "receiver administration"):
            provision.configure_uplink(tx, provision.uplink_fields(*UPLINK))
        self.assertEqual(["HELLO"], [verb for _, verb in self.events])

    def test_status_shapes(self):
        self.assertEqual({"configured": False}, provision.uplink_info(self.rx, "0000000000000001"))
        for info in (["1", "host", "port", "user"], ["2", "h", "1", "u"], ["1", "h", "1"]):
            self.rx.info = info
            with self.assertRaisesRegex(provision.ProvisioningError, "Invalid uplink status"):
                provision.uplink_info(self.rx, "0000000000000001")

    def test_local_validation_rejects_values_the_device_would_refuse(self):
        cases = (
            (0, "", "SSID"),
            (0, "s" * 33, "SSID"),
            (0, "a\0b", "SSID"),
            (1, "short", "Wi-Fi password"),
            (1, "p" * 65, "Wi-Fi password"),
            (1, "password\0", "Wi-Fi password"),
            (2, "bad host", "host"),
            (3, 0, "port"),
            (3, 65536, "port"),
            (4, "bad/source", "username"),
            (4, "-leading", "username"),
            (5, "", "MQTT password"),
            (5, "p" * 65, "MQTT password"),
            (5, "x\0", "MQTT password"),
        )
        for index, value, message in cases:
            with self.subTest(index=index, value=value):
                values = list(UPLINK)
                values[index] = value
                with self.assertRaisesRegex(provision.ProvisioningError, message):
                    provision.uplink_fields(*values)
        fields = dict(provision.uplink_fields("Rede é", "p" * 64, "broker.local", 65535, "a", "m"))
        self.assertEqual("65535", fields["port"])

    def test_secrets_come_from_the_first_file_line_or_a_silent_prompt(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "secret"
            path.write_text("line one\r\nline two\n")
            self.assertEqual("line one", provision.read_secret(path, "unused"))
        with patch.object(provision.getpass, "getpass", return_value="typed") as prompt:
            self.assertEqual("typed", provision.read_secret(None, "Wi-Fi password: "))
        prompt.assert_called_once_with("Wi-Fi password: ")


class UplinkCommandLineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.secret = Path(self.temporary.name) / "mqtt"
        self.secret.write_text("mqtt secret\n")
        self.devices = {"rx-port": Device("rx", "0000000000000001", [])}

    def tearDown(self):
        self.temporary.cleanup()

    def run_cli(self, *arguments):
        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            patch.object(provision, "SerialLink", side_effect=self.devices.__getitem__),
            patch.object(provision.getpass, "getpass", return_value="typed wifi"),
            patch.object(sys, "argv", ["provision.py", *arguments]),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            code = provision.main()
        return code, stdout.getvalue(), stderr.getvalue()

    def test_uplink_and_status_print_json_without_secrets(self):
        code, output, _ = self.run_cli(
            "uplink",
            "--receiver",
            "rx-port",
            "--ssid",
            "Bench net",
            "--host",
            "192.168.1.20",
            "--username",
            "receiver-1",
            "--mqtt-password-file",
            str(self.secret),
        )
        self.assertEqual(0, code)
        self.assertEqual(1883, json.loads(output)["port"])
        self.assertNotIn("secret", output)
        self.assertNotIn("typed", output)
        self.assertEqual("typed wifi", self.devices["rx-port"].uplink["wifipass"])
        code, output, _ = self.run_cli("uplink-status", "--receiver", "rx-port")
        self.assertEqual(0, code)
        self.assertEqual("receiver-1", json.loads(output)["username"])
        self.assertTrue(self.devices["rx-port"].closed)

    def test_invalid_uplink_fails_before_opening_the_port(self):
        code, output, error = self.run_cli(
            "uplink",
            "--receiver",
            "rx-port",
            "--ssid",
            "Bench net",
            "--host",
            "bad host",
            "--username",
            "receiver-1",
            "--mqtt-password-file",
            str(self.secret),
        )
        self.assertEqual(1, code)
        self.assertIn("host", error)
        self.assertEqual("", output)
        self.assertFalse(self.devices["rx-port"].closed)
