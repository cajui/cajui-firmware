# USB enrollment

Each radio image (`runtime_tx`, `runtime_rx`) has a USB administration mode for the
Heltec WiFi LoRa 32 V3 (ESP32-S3). In admin mode the SX1262 stays in reset: nothing is
transmitted, so enrollment is safe without an antenna, and credentials never change
while a frame is in flight. A device boots in admin mode when it has no usable
enrollment or storage, or when the previous boot asked for it with `CJ1 ADMIN`.
Otherwise it runs its radio application, where the console answers only read-only
queries (`HELLO`, `INFO`, `UPLINKINFO`) and `ADMIN`. `REBOOT` leaves admin mode.
The tool switches modes itself: it wakes a sleeping transmitter by pulsing RTS (the
board's reset line), requests `ADMIN`, waits for the new boot, and sends `REBOOT` when
it is done.

## Build and install

Install PlatformIO 6.1.18. Confirm the target and stable MAC of each physical device
before uploading; serial port names and USB-bridge serial numbers are not reliable
identities. Back up existing flash/state before changing the partition layout.
The images use the [dedicated storage layout](persistence.md). Released images install
from the [web installer](updates.md#web-installer-usb) without PlatformIO; a receiver also
accepts [signed updates](updates.md#signed-updates-setup-page) from its setup page.

```sh
# Replace these placeholders with the independently identified serial ports.
pio run -e runtime_tx -t upload --upload-port <transmitter-port>
pio run -e runtime_rx -t upload --upload-port <receiver-port>
```

A new device starts in admin mode and waits for enrollment.

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

This does not erase the node's local key or old queued readings. Revoked generations
remain in storage to decode queued data; when a slot is needed, one without queued
samples is freed and its credential retired so it can never return. See
[storage limits](persistence.md#limits-and-retained-history).

To move a device to another network, or to discard its enrollment entirely:

```sh
uv run tools/provision.py reset --port <port>
```

The device retires every enrollment and forgets its network, then accepts a new USB
enrollment or radio pairing. Its old keys can never be enrolled again, so any recovery
file for it becomes useless. A receiver with queued samples refuses unless
`--discard-queue` is given. Revoke the device's binding on the other side as well.

## Serial protocol CJ1

ASCII commands end in LF, at most 255 bytes excluding LF. Fields are separated by
exactly one space. Embedded control characters, CRLF, extra fields and overlong lines
are rejected. Overflow discards the complete line before accepting another command.
Requests are never echoed. Non-HELLO operations include the expected 16-digit device
ID to prevent accidental writes to a swapped port. `HELLO` ends with the mode, `admin` or
`run`; older admin-only images omitted it. `CJ1 ADMIN <device>` restarts into admin mode
from either mode; in operation, other mutations answer `CJ1 ERR ADMIN`. `CJ1 PAIR
<device>` restarts a transmitter into [radio pairing](radio-pairing.md) from either mode;
`uv run tools/provision.py pair --transmitter <port>` sends it, waking the node first. Identifiers are lowercase fixed
width hex, not decimal JSON numbers.

```text
CJ1 HELLO
CJ1 PREPARE <device:16> <network:16> <receiver:16> <node:16> <generation:16> <key:32> <profile:4>
CJ1 ACTIVATE <device:16> <node:16> <generation:16>
CJ1 INFO <device:16> <node:16> <generation:16>
CJ1 REVOKE <device:16> <node:16> <generation:16>
CJ1 RESERVE <device:16> <node:16> <generation:16>
CJ1 RESET <device:16> [discard]
CJ1 POWER <device:16> [dbm]
CJ1 REBOOT <device:16>
```

Responses are `CJ1 OK <command> [fields]` or `CJ1 ERR <code>`. No response returns a
key. HELLO fields: device, role (`tx`/`rx`), health, network, receiver, profile,
queue count in decimal, ephemeral boot ID (8 hex digits). Health is `ready` or the
reason storage is unusable: `unmounted` (NVS did not open), `identity` (invalid
device ID or role), `read`, `corrupt` (size or CRC), `format` (unknown
magic/version), `role` or `device` (the stored state belongs to another role or device,
for example after uploading the wrong image), `invalid` (semantic validation) or
`write` (a failed or ambiguous write since boot). A boot ID is diagnostic, not an
authentication token or GCM nonce. INFO returns enrollment state (prepared=1,
active=2, revoked=3) and last reserved counter (16 hex digits). RESERVE is an
administrative persistence probe, not a radio-send operation. While storage is
unhealthy, the enrollment commands (PREPARE, ACTIVATE, INFO, REVOKE, RESERVE, RESET) return
`STORAGE`; HELLO, REBOOT, ADMIN, PAIR, POWER and the UPLINK commands do not depend on the
protocol records and stay available. REBOOT matters most, because restarting remounts the
store. INFO, ACTIVATE, REVOKE and
RESERVE return `NOT_FOUND` for an unknown node/generation pair. RESERVE returns
`INVALID` on a receiver and `CONFLICT` for a prepared or revoked generation. RESET
returns `QUEUED` while a receiver holds samples, unless `discard` is given. POWER without
a value reports the configured transmit power (the −9 dBm default when none is stored)
in both modes, or `STORAGE` if the `cajui` partition could not be opened; with a signed
decimal value from −9 to 22, without leading zeros, it stores it, in admin mode only. It
applies at the next restart.

Profile `0001` is the initial direct-LoRa profile identifier; its field and regulatory
validation remain pending. Radio pairing is the over-the-air alternative to USB
enrollment. There is no remote administration: every CJ1 command needs USB access.

## Transmit power

```sh
uv run tools/provision.py power --port <port>             # report
uv run tools/provision.py power --port <port> --dbm 14    # store, then restart
```

The value is stored in the `radio` record of the `cajui` partition, separately from
enrollment. It is the maximum a receiver's power command can use on a transmitter; see
[radio applications](radio-applications.md#enrollment-and-image-changes). Choosing a
value that the region and antenna permit is the operator's responsibility.

## Receiver uplink settings

The receiver forwards queued samples to an MQTT broker when Wi-Fi and broker settings
are stored. The [setup page](radio-applications.md#receiver-setup-page) configures them
without a computer. Over USB, the tool switches the receiver to admin mode and back:

```sh
uv run tools/provision.py uplink --receiver <rx-port> \
  --ssid "network name" --host 192.168.1.20 --port 1883 \
  --username receiver-1 --mqtt-password-file path/to/broker-password
uv run tools/provision.py uplink-status --receiver <rx-port>
```

Omitting `--wifi-password-file` or `--mqtt-password-file` prompts without echo. Files
contribute their first line. The MQTT username is also the `source_id` in
[Cajuí Central's contract](https://github.com/cajui/cajui-central#wire-contract-version-1),
so a broker ACL that grants each user `telemetry/v1/<user>/+/samples` authorizes it.
Every field is required: open Wi-Fi networks and 5 GHz-only networks are not
supported (the ESP32-S3 has no 5 GHz radio). Output reports the stored host, port and
username, never a password, and `forwarding_validated: false`.

CJ1 commands, receiver only: `UPLINKSET <device> <field> <hex>` stages one field
(`ssid`, `wifipass`, `host`, `port`, `user`, `pass`) as lower-case hex of its bytes, so
spaces and non-ASCII characters survive the space-delimited protocol. `UPLINKSAVE
<device>` validates the complete set and writes it atomically to the `uplink` key of
the `cajui` NVS partition, separate from the protocol records; staging is wiped on
success. `UPLINKINFO <device>` returns `0` when nothing is stored or `1 <host> <port>
<user>`. Stored secrets, including Wi-Fi and broker passwords, are not encrypted at
rest, like enrollment keys. Anyone with USB access can replace them.
