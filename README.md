# Cajuí Firmware

[![CI](https://github.com/cajui/cajui-firmware/actions/workflows/ci.yml/badge.svg)](https://github.com/cajui/cajui-firmware/actions/workflows/ci.yml)

Firmware foundations for a direct LoRa telemetry network: sensor nodes send readings
to a receiver, which acknowledges accepted samples and eventually forwards them to
a server.

**Experimental development code.** The shared protocol core, persistent storage, USB
enrollment and host-tested delivery controllers are implemented. Experimental ESP32
transmitter/receiver applications integrate an SX1262 adapter and a DHT22 sensor.
Each image has a USB administration mode that keeps the radio in reset. No production image is
released.

## Implemented

- Bounded DATA/ACK frames with up to eight sensor metrics and AES-128-GCM.
- Per-device credentials, replay/duplicate handling and bounded sender attempts.
- Versioned per-record storage with an ESP32 NVS adapter, durable counter reservation,
  a 128-frame receiver queue committed before ACK in two small writes per sample,
  reusable enrollment slots and migration from the earlier single-snapshot layout.
- Wire version 2: the ACK can command the transmitter's power (clamped to its configured
  maximum, with a fallback after missed ACKs); version 1 nodes keep working. No power
  policy is enabled yet. Transmit power is configurable per device over USB.
- Radio RSSI/SNR of each accepted frame, logged and forwarded as `radio` readings.
- Two application slots with rollback: a browser installer over USB for released images,
  and signed updates installed from the receiver's setup page ([updates](docs/updates.md)).
- Two-phase USB enrollment, resumable setup, key rotation, revocation and leaving a
  network (retired keys can never return).
- A local Python tool with private recovery files and a software-restart check.
- A nonblocking send controller with injected radio, clock and jitter, bounded
  channel waits, ACK deadlines and cancellation.
- SX1262 CAD/TX/RX adapter with task-driven interrupt handling, a durable receiver
  loop, DHT22 sampling and a five-minute transmitter sleep schedule.
- Receiver forwarding to an MQTT broker over Wi-Fi in Cajuí Central's JSON contract,
  removing each queued sample only after its PUBACK, with USB-provisioned settings.
- A receiver setup page on a temporary access point opened by holding the PRG button:
  Wi-Fi, MQTT broker (with mDNS discovery), status and transmitter revocation, applied
  without a reboot. The OLED shows a QR code to join the setup network.
- [Radio pairing](docs/radio-pairing.md): add a transmitter from the setup page with an
  X25519/HKDF key exchange; the transmitter joins after a long PRG press or `CJ1 PAIR`.
- Unity tests, Python client tests, ASan/UBSan and coverage checks.

## Pending and unvalidated

This list is the single record of implementation status; the other documents describe
contracts and link here.

- Field battery-voltage/power policy. Runtime battery readings are explicitly unknown;
  use USB for development. Without uplink settings, or while the broker is unreachable,
  the receiver stops accepting new samples once its 128-frame durable queue is full.
- **TODO (security): the setup access point is open.** Anyone within Wi-Fi range who
  joins it while it is open (at most 30 minutes, after a button press) can change the
  uplink settings, pair or revoke transmitters. The page itself refuses other origins,
  other hosts and the home-network interface, and never reveals or re-sends a stored
  password. A per-device password on a label/QR is planned.
- **Radio pairing is not authenticated against an active attacker** in radio range during
  the two-minute window; a per-device label code is planned. Signal strength is shown,
  not enforced.
- Broker TLS and an application-level receipt from Central. Forwarding uses plain MQTT
  3.1.1 on a trusted network; PUBACK proves broker acceptance only.
- Physical validation is limited: a manual bench exchange achieved durable acceptance
  and authenticated ACKs on the first attempt, including sensor-error telemetry followed
  by valid climate readings on a subsequent boot. The queue survived that restart;
  one automatic five-minute wake also delivered valid readings and received an ACK.
  IRQ timing, loss/interference, sustained cadence and power use need measurement. CI only
  compiles the ESP32 targets; USB enrollment alone does not validate RF.
- Arbitrary power-loss behavior and flash endurance of the NVS adapter. Host tests inject
  storage failures; the adapter relies on NVS atomic blob replacement.
- RF coexistence, regulatory configuration and an independent security review.

[Runtime architecture](docs/runtime.md) · [USB administration](docs/provisioning.md) ·
[Persistent storage](docs/persistence.md) · [Radio applications](docs/radio-applications.md) ·
[Firmware updates](docs/updates.md) · [MQTT management channel (draft)](docs/management-v1.md)

## Run tests

Requires Python 3, a C/C++ toolchain, OpenSSL development headers, and either
PlatformIO 6.1.18 or `uv` (the script uses `uvx` when `pio` is absent).

```sh
# macOS dependency
brew install openssl@3
# Ubuntu/Debian alternative
# sudo apt-get install build-essential libssl-dev

python3 -m pip install platformio==6.1.18
python3 scripts/check_protocol.py
python3 scripts/check_protocol.py --lint
```

`--lint` checks formatting and runs clang-tidy on `lib/` and ruff on the Python code. It
uses `uvx` to run the pinned clang-format 19.1.7, clang-tidy 19.1.0 and ruff 0.6.9; without
`uv`, install those versions with pip.

For LLVM coverage, install Clang and LLVM (Xcode command-line tools on macOS):

```sh
CC=clang CXX=clang++ python3 scripts/check_protocol.py --coverage
```

The coverage gate applies to each host implementation file listed in
`scripts/check_protocol.py` on its own: at least 95% line and 85% branch coverage, with one
documented exception for OpenSSL failure branches in `crypto.cpp`. The Python client needs 95%
line and branch coverage. Coverage does not measure the ESP32 backend, radio behavior or
the NVS backend itself. See [testing](docs/testing.md).

Compile the same tests for ESP32 without uploading or executing them:

```sh
pio test -e protocol_esp32 --without-uploading --without-testing
```

This builds a test image, not an operational node. The current compile target is
ESP32-S3; the protocol core does not depend on a radio driver.

## Structure

```text
lib/CajuiProtocol/src/    Shared wire format, sender/receiver logic and crypto adapters
lib/CajuiApplication/src/ Receiver loop and measurement normalization
lib/CajuiRuntime/src/     Send-cycle state machine and radio/clock/jitter contracts
lib/CajuiStorage/src/     Persistent records, v1 migration and NVS adapter
lib/CajuiProvisioning/src/ Bounded USB command handler
lib/CajuiPairing/src/     Radio pairing frames and state machines
lib/CajuiUplink/src/      Uplink settings, Central JSON formatting and MQTT forwarding
lib/CajuiSetup/src/       Setup page rendering, session checks and field validation
lib/CajuiDevice/src/      Boot-mode, fault-retry and transmit-power decisions
lib/CajuiFirmware/src/    Signed firmware update verification
src/                     Transmitter and receiver applications and board adapters
tools/                   USB enrollment client and firmware packaging
site/                    Web installer page, published by the release workflow
scripts/                 Native build configuration and test runner
test/test_protocol/      Unity tests and fault-injection storage doubles
tests_python/            USB client and recovery-file tests
docs/                    Protocol contract, testing and integration roadmap
```

[Protocol specification](docs/protocol-v1.md) · [Roadmap](docs/roadmap.md) ·
[Contributing](CONTRIBUTING.md) · [Security](SECURITY.md)

[Cajuí Central](https://github.com/cajui/cajui-central) is the separate monitoring
application. The receiver publishes to the MQTT broker that Central consumes.

## License

Apache-2.0. See [LICENSE](LICENSE). Dependencies retain their respective licenses;
Unity, OpenSSL and the ESP32 framework are fetched through the build environment,
not vendored here. Test keys are public fixtures, never provisioning credentials.
