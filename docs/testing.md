# Testing

Unity 2.6.1 runs the shared C++11 suite through PlatformIO. Native tests use real
OpenSSL AES-GCM with ASan/UBSan. Protocol tests use journal/counter doubles; storage
tests exercise the real store over an in-memory record adapter that injects failures
before, during and after each write, including ambiguous and torn writes.
This is not a flash simulation with a proven power-loss model. The NVS adapter and
radio images are also compiled, without physical access during CI.

The suite covers multiple metrics, zero and negative readings, field limits,
header serialization, truncation, tampering with each packet byte, wrong keys and
bindings, authenticated malformed payloads, deterministic random malformed input,
replay/conflicting counters, lost ACKs, duplicate delivery, full queues, failed
persistence, bounded attempts and counter exhaustion. A known AES-GCM vector
checks the crypto adapter independently of codec round trips.

```sh
python3 scripts/check_protocol.py --lint
python3 scripts/check_protocol.py
CC=clang CXX=clang++ python3 scripts/check_protocol.py --coverage
pio test -e protocol_esp32 --without-uploading --without-testing
```

`--lint` runs clang-format, clang-tidy and ruff with pinned versions. Library code uses
`.clang-tidy`; tests use the bug-finding subset in `test/.clang-tidy`, since fixtures and
record offsets are deliberate literals. `src/` is not analyzed because it needs the
Arduino headers. PyPI has no clang-tidy 19.1.0 wheel for Linux on ARM64, where pip
would build LLVM from source; on such machines, run `--lint` on macOS or in an x86-64
container.
On macOS the runner locates OpenSSL via Homebrew and LLVM via `xcrun`. Elsewhere,
OpenSSL and LLVM must be on the compiler/tool search paths. `OPENSSL_ROOT_DIR`
can specify a custom OpenSSL installation. PlatformIO/Unity versions are pinned.

The coverage gate applies **to each file on its own**, never to an aggregate: every host
implementation file listed in `GATED_FILES` of `scripts/check_protocol.py` (codec,
delivery, runtime, application, crypto, storage, records, v1 snapshot, CRC32,
provisioning, uplink, setup, pairing and device decisions) needs 95% lines and 85%
branches. One exception is documented in the script: the host branch coverage of
`crypto.cpp` needs 60%, because its remaining branches are OpenSSL allocation and EVP
failure returns that no test can trigger without fault injection into the library;
known-answer vectors cover its success paths. A file missing from the report fails the
gate. Uplink tests
check the exact Central JSON, the settings blob and PUBACK-gated queue removal against a
publisher double; the Wi-Fi/MQTT adapter itself is only compiled. Pairing tests check X25519 and HKDF against RFC 7748 and RFC 5869, tampering with every
offer byte, a complete exchange between the node and receiver state machines followed by
accepted DATA, rotation, window expiry, lost JOIN_DONE and a foreign network. The ESP32
backends (mbedTLS X25519, HKDF composed from mbedTLS HMAC) were checked against the same
vectors with a separate probe program on a board. Running this Unity suite on the board
is not part of validation. Setup tests cover button timing, the
Wi-Fi QR code, field staging, HTML escaping, session tokens and limits, host and origin
checks, notice codes and oversized pages; the access point, DNS, HTTP server, scanning
and mDNS discovery run only on hardware. Device tests cover boot-mode selection and the
fault retry delay; the Arduino entry points that use them are only compiled. Compiler/library allocation
failures are not all induced. Neither a high coverage percentage nor a passing ESP32
build proves security, radio performance, durable flash behavior or battery life.

The ESP32 target compiles the same tests with mbedTLS and explicit
`UNITY_SUPPORT_64` for the protocol counters and identities. Build-only success is not a
physical test result. CI does not connect to devices or upload firmware.

Python unittest tests exercise client sequencing, resumable setup, identity checks,
recovery-file permissions, error redaction and the command line. With `--coverage`, the
check script also requires 95% line and branch coverage of `tools/provision.py`
(coverage.py 7.6.1, run through `uvx` or installed with pip).
The gate reads the client file's JSON counts and checks each metric independently,
without rounding. A high combined percentage cannot compensate for low branch
coverage. Regression tests cover that distinction and missing coverage data.
The build job also compiles `runtime_tx` and `runtime_rx`. No CI step uploads a device.

Controller tests use a simulated clock, radio and jitter source, including time
rollover, completion timestamps, cancellation, driver failures and invalid ACKs.
A fixed pre-refactor wire fixture checks compatibility in addition to round trips.
Coverage includes the send and receive controllers and measurement normalization.
It excludes the board application, SX1262 adapter, FreeRTOS scheduling and sensor
driver. Those need physical tests; compile success is not timing validation.
