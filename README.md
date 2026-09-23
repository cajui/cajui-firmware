# Cajuí Firmware

[![CI](https://github.com/cajui/cajui-firmware/actions/workflows/ci.yml/badge.svg)](https://github.com/cajui/cajui-firmware/actions/workflows/ci.yml)

Firmware foundations for a direct LoRa telemetry network: sensor nodes send readings
to a receiver, which acknowledges accepted samples and eventually forwards them to
a server.

**Experimental development code.** The shared protocol core, persistent storage, USB
enrollment and a host-tested delivery scheduler are implemented. Administration-only
ESP32 images are available; they keep the radio in reset. No production transmitter or
receiver image is released.

## Implemented

- Bounded DATA/ACK frames with up to eight sensor metrics and AES-128-GCM.
- Per-device credentials, replay/duplicate handling and bounded sender attempts.
- Versioned snapshot storage with an ESP32 NVS adapter, durable counter reservation,
  a 128-frame receiver queue and atomic queue/receipt updates before ACK.
- Two-phase USB enrollment, resumable setup, key rotation and revocation.
- A local Python tool with private recovery files and a software-restart check.
- A nonblocking send controller with injected radio, clock and jitter, bounded
  channel waits, ACK deadlines and cancellation.
- Unity tests, Python client tests, ASan/UBSan and coverage checks.

## Pending and unvalidated

This list is the single record of implementation status; the other documents describe
contracts and link here.

- Radio adapter, sensor drivers, receiver radio loop and server forwarding.
- Execution on hardware: CI only compiles the ESP32 targets, and no physical radio
  exchange has been tested. The USB restart check does not establish radio communication.
- Arbitrary power-loss behavior and flash endurance of the NVS adapter. Host tests inject
  storage failures; the adapter relies on NVS atomic blob replacement.
- RF coexistence, regulatory configuration and an independent security review.

[Runtime architecture](docs/runtime.md) · [USB administration](docs/provisioning.md) ·
[Persistent storage](docs/persistence.md)

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

Coverage gates apply to the host implementation files listed in
`scripts/check_protocol.py` (codec, crypto, delivery, runtime, storage, snapshot and
command handler): at least 95% line and 85% branch coverage. The Python client needs 95%
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
lib/CajuiRuntime/src/     Send-cycle state machine and radio/clock/jitter contracts
lib/CajuiStorage/src/     Persistent state machine and NVS adapter
lib/CajuiProvisioning/src/ Bounded USB command handler
src/                     Radio-disabled ESP32 administration application
tools/                   Local USB enrollment client
scripts/                 Native build configuration and test runner
test/test_protocol/      Unity tests and fault-injection storage doubles
tests_python/            USB client and recovery-file tests
docs/                    Protocol contract, testing and integration roadmap
```

[Protocol specification](docs/protocol-v1.md) · [Roadmap](docs/roadmap.md) ·
[Contributing](CONTRIBUTING.md) · [Security](SECURITY.md)

[Cajuí Central](https://github.com/cajui/cajui-central) is the separate monitoring
application. A bridge to its API is planned, not available in this initial version.

## License

Apache-2.0. See [LICENSE](LICENSE). Dependencies retain their respective licenses;
Unity, OpenSSL and the ESP32 framework are fetched through the build environment,
not vendored here. Test keys are public fixtures, never provisioning credentials.
