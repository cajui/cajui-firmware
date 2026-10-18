# Development

[Back to Cajuí Firmware](../README.md)

## Run tests

Requires Python 3.10+, a C/C++ toolchain, OpenSSL development headers, and either
PlatformIO 6.2.0 or `uv` (the script uses `uvx` when `pio` is absent).

```sh
# macOS dependency
brew install openssl@3
# Ubuntu/Debian alternative
# sudo apt-get install build-essential libssl-dev

uv run --python 3.12 python scripts/check_protocol.py
uv run --python 3.12 python scripts/check_protocol.py --lint
```

`--lint` checks formatting, clang-tidy, Python, shell scripts and workflow YAML.
It uses `uvx` to run pinned clang-format, clang-tidy, ruff, ShellCheck and actionlint.
The actionlint environment also includes ShellCheck for embedded workflow scripts.
Without `uv`, create a virtual environment and install the versions listed in
`.github/requirements/lint.in` before running the script.

For LLVM coverage, install Clang and LLVM (Xcode command-line tools on macOS):

```sh
CC=clang CXX=clang++ uv run --python 3.12 --with coverage==7.16.2 \
  python scripts/check_protocol.py --coverage
```

The coverage gate applies to each host implementation file listed in
`scripts/check_protocol.py` on its own: at least 95% line and 85% branch coverage, with
one documented exception for OpenSSL failure branches in `crypto.cpp`. The Python client
and release tools each need 95% line and branch coverage using the same interpreter
as the ordinary tests. Coverage does not measure the ESP32 backend, radio behavior or
the NVS backend itself. See [testing](testing.md).

Compile the same tests for ESP32 without uploading or executing them:

```sh
uvx --from platformio==6.2.0 pio test -e protocol_esp32 --without-uploading --without-testing
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

[Protocol specification](protocol-v1.md) · [Roadmap](roadmap.md) ·
[Contributing](../CONTRIBUTING.md) · [Security](../SECURITY.md)

[Cajuí Central](https://github.com/cajui/cajui-central) is the separate monitoring
application. The receiver publishes to the MQTT broker that Central consumes.
