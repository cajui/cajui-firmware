# Cajuí Firmware

[![CI](https://github.com/cajui/cajui-firmware/actions/workflows/ci.yml/badge.svg)](https://github.com/cajui/cajui-firmware/actions/workflows/ci.yml)

Firmware foundations for a direct LoRa telemetry network: sensor nodes send readings
to a receiver, which acknowledges accepted samples and eventually forwards them to
a server.

**Experimental, pre-integration code.** This initial repository contains the shared
C++11 protocol core and tests. It does not yet provide a deployable transmitter or
receiver application. Persistent storage, USB provisioning, radio integration and
server forwarding are not implemented. No production firmware binaries are released.

## Implemented

- A bounded binary DATA/ACK format with up to eight sensor metrics per sample.
- AES-128-GCM using OpenSSL on the host and mbedTLS on ESP32.
- Per-device binding, authenticated headers and acknowledgement correlation.
- Duplicate/replay handling through an injected persistence contract.
- Up to three attempts per pending sample, with explicit abandonment.
- Unity unit tests, address/undefined-behavior sanitizers and coverage checks.

The persistence implementations in tests are in-memory doubles. The core requires
an atomic, durable queue/receipt commit before acknowledging a new sample; this
contract is not a claim that flash storage has already been implemented or tested.

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
```

For LLVM coverage, install Clang and LLVM (Xcode command-line tools on macOS):

```sh
CC=clang CXX=clang++ python3 scripts/check_protocol.py --coverage
```

Coverage gates apply to the two host implementation files: at least 95% line
coverage and 85% branch coverage. Coverage does not measure the ESP32 backend,
radio behavior or durable storage. See [testing](docs/testing.md).

Compile the same tests for ESP32 without uploading or executing them:

```sh
pio test -e protocol_esp32 --without-uploading --without-testing
```

This builds a test image, not an operational node. The current compile target is
ESP32-S3; the protocol core does not depend on a radio driver.

## Structure

```text
lib/CajuiProtocol/src/    Shared wire format, sender/receiver logic and crypto adapters
scripts/                 Native build configuration and test runner
test/test_protocol/      Unity tests and persistence doubles
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
