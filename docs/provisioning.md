# USB enrollment

The `admin_tx` and `admin_rx` images provide USB administration and persistent
storage on the Heltec WiFi LoRa 32 V3 (ESP32-S3) compile target. They hold SX1262 in
reset and do not link a radio driver. They do not sample sensors or forward data.
These images replace any application currently running on the device.

## Build and install

Install PlatformIO 6.1.18. Confirm the target and stable MAC of each physical device
before uploading; serial port names and USB-bridge serial numbers are not reliable
identities. Back up existing flash/state before changing the partition layout.
The images use the [dedicated storage layout](persistence.md), not an OTA update.

```sh
pio run -e admin_tx -e admin_rx
# Replace these placeholders with the independently identified serial ports.
pio run -e admin_tx -t upload --upload-port <transmitter-port>
pio run -e admin_rx -t upload --upload-port <receiver-port>
```

Do not upload the Unity test image as an application. CI never accesses devices.

## Enroll, resume and verify

The tool requires Python 3.10+ and pyserial 3.5. With `uv`, the script metadata installs
the dependency automatically. Alternatively install `pyserial==3.5` and use Python.
The current client targets macOS/Linux: it uses POSIX ownership checks, file modes
and directory fsync. It is not a Windows installer.

```sh
uv run tools/provision.py status --port <port>
uv run tools/provision.py enroll --transmitter <tx-port> --receiver <rx-port> \
  --state /path/to/private/enrollment.json
uv run tools/provision.py resume --transmitter <tx-port> --receiver <rx-port> \
  --state /path/to/private/enrollment.json
uv run tools/provision.py verify-restart --transmitter <tx-port> --receiver <rx-port> \
  --state /path/to/private/enrollment.json
```

The state path above is a placeholder: use an existing private directory outside
version control. Each node gets its own recovery file. The file contains its secret
key and must be retained privately, not attached to issues or committed. New files
are created exclusively with mode 0600; updates use fsync and atomic replacement.
Resume rejects world/group-readable files and symlinks. CLI output does not include
the key. USB requests do carry it, so do not record a full serial session during
setup. Anyone with access to this serial endpoint can administer the device.

Enrollment generates a random network ID if the receiver has none, a random
credential generation and a unique random 128-bit key. Existing receiver network
and profile must agree with the node. The tool saves recovery state **before** any
device mutation, prepares the receiver and transmitter, activates the receiver,
then activates the transmitter. Every phase is resumable with the same file.
Repeating preparation/activation of the same generation never resets counters.
Attempting to reuse a revoked generation/key is rejected.

There is no distributed atomic commit between two USB devices. A disconnect after
receiver activation can temporarily leave devices on different generations; resume
finishes the intended transaction. A failed transaction is never reported as fully
configured. Do not discard its recovery file and blindly start another enrollment;
resume or explicitly revoke the pending generation first.

`verify-restart` checks both enrollments, durably reserves a counter, requests a
software restart of both devices, observes new boot identifiers, checks persisted
enrollment, and reserves a strictly higher counter. These reservations are consumed
without transmitting. `configured` and `restart_verified` do **not** imply a tested
radio link: the tool reports `radio_validated: false`.

To revoke receiver authorization:

```sh
uv run tools/provision.py revoke --receiver <rx-port> \
  --state /path/to/private/enrollment.json
```

This does not erase the node's local key or old queued readings. Old generations
remain in storage to prevent reuse and to decode queued data. The registry currently
holds at most 16 generations; see storage limits before repeated rotations.

## Serial protocol CJ1

ASCII commands end in LF, at most 255 bytes excluding LF. Fields are separated by
exactly one space. Embedded control characters, CRLF, extra fields and overlong lines
are rejected. Overflow discards the complete line before accepting another command.
Requests are never echoed. Non-HELLO operations include the expected 16-digit device
ID to prevent accidental writes to a swapped port. Identifiers are lowercase fixed
width hex, not decimal JSON numbers.

```text
CJ1 HELLO
CJ1 PREPARE <device:16> <network:16> <receiver:16> <node:16> <generation:16> <key:32> <profile:4>
CJ1 ACTIVATE <device:16> <node:16> <generation:16>
CJ1 INFO <device:16> <node:16> <generation:16>
CJ1 REVOKE <device:16> <node:16> <generation:16>
CJ1 RESERVE <device:16> <node:16> <generation:16>
CJ1 REBOOT <device:16>
```

Responses are `CJ1 OK <command> [fields]` or `CJ1 ERR <code>`. No response returns a
key. HELLO fields: device, role (`tx`/`rx`), health, network, receiver, profile,
queue count in decimal, ephemeral boot ID (8 hex digits). Health is `ready` or the
reason storage is unusable: `unmounted` (NVS did not open), `identity` (invalid
device ID or role), `read`, `corrupt` (size or CRC), `format` (unknown
magic/version), `role` or `device` (the snapshot belongs to another role or device,
for example after uploading the wrong image), `invalid` (semantic validation) or
`write` (a failed or ambiguous write since boot). A boot ID is diagnostic, not an
authentication token or GCM nonce. INFO returns enrollment state (prepared=1,
active=2, revoked=3) and last reserved counter (16 hex digits). RESERVE is an
administrative persistence probe, not a radio-send operation. While storage is
unhealthy, well-formed commands other than HELLO and REBOOT return `STORAGE`; REBOOT
stays available because restarting remounts the store. INFO, ACTIVATE, REVOKE and
RESERVE return `NOT_FOUND` for an unknown node/generation pair. RESERVE returns
`INVALID` on a receiver and `CONFLICT` for a prepared or revoked generation.

Profile `0001` is the initial direct-LoRa profile identifier; radio integration and
its field/regulatory validation remain pending. No over-the-air enrollment, remote
administration authentication, network migration or reset command is provided.
