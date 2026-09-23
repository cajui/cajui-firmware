# Testing

Unity 2.6.1 runs the shared C++11 suite through PlatformIO. Native tests use real
OpenSSL AES-GCM with ASan/UBSan. Protocol tests use journal/counter doubles; storage
tests exercise the real snapshot state machine over a fault-injection blob adapter.
This is not a flash simulation with a proven power-loss model. The NVS adapter and
administration images are also compiled, without physical access during CI.

The suite covers multiple metrics, zero and negative readings, field limits,
header serialization, truncation, tampering with each packet byte, wrong keys and
bindings, authenticated malformed payloads, deterministic random malformed input,
replay/conflicting counters, lost ACKs, duplicate delivery, full queues, failed
persistence, bounded attempts and counter exhaustion. A known AES-GCM vector
checks the crypto adapter independently of codec round trips.

```sh
python3 scripts/check_protocol.py
CC=clang CXX=clang++ python3 scripts/check_protocol.py --coverage
pio test -e protocol_esp32 --without-uploading --without-testing
```

On macOS the runner locates OpenSSL via Homebrew and LLVM via `xcrun`. Elsewhere,
OpenSSL and LLVM must be on the compiler/tool search paths. `OPENSSL_ROOT_DIR`
can specify a custom OpenSSL installation. PlatformIO/Unity versions are pinned.

Coverage gates include `cajui_protocol.cpp`, the host branch of `crypto.cpp`,
`cajui_storage.cpp` and `cajui_provisioning.cpp`: 95% lines and 85% branches. Compiler/library allocation failures are
not all induced. Neither a high coverage percentage nor a passing ESP32 build
proves security, radio performance, durable flash behavior or battery life.

The ESP32 target compiles the same tests with mbedTLS. Build-only success is not a
physical test result. CI does not connect to devices or upload firmware.

Python unittest tests exercise client sequencing, resumable setup, identity checks,
recovery-file permissions and error redaction. The shared check script runs them.
The build job also compiles `admin_tx` and `admin_rx`. No CI step uploads a device.
