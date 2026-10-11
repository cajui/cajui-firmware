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
record offsets are deliberate literals. `src/` is analyzed separately after the ESP32 builds, using their Arduino headers
and compiler flags (see Board lint below). PyPI has no clang-tidy 19.1.0 wheel for Linux on ARM64, where pip
would build LLVM from source; on such machines, run `--lint` on macOS or in an x86-64
container.
On macOS the runner locates OpenSSL via Homebrew and LLVM via `xcrun`. Elsewhere,
OpenSSL and LLVM must be on the compiler/tool search paths. `OPENSSL_ROOT_DIR`
can specify a custom OpenSSL installation. PlatformIO/Unity versions are pinned.

The coverage gate applies **to each file on its own**, never to an aggregate: every host
implementation file listed in `GATED_FILES` of `scripts/check_protocol.py` (codec,
delivery, runtime, application, crypto, storage, records, v1 snapshot, CRC32,
provisioning, uplink, setup, pairing, firmware updates, SHT4x, device decisions and
service safety and setup-save retries) needs 95% lines and 85% branches. One exception
is documented in the script: the host branch coverage of `crypto.cpp` needs 60%, because its remaining branches are OpenSSL
allocation and EVP failure returns that no test can trigger without fault injection into
the library; known-answer vectors cover its success paths. A file missing from the
report fails the gate. Uplink tests check the exact Central JSON, the settings blob and
PUBACK-gated queue removal against a publisher double; the Wi-Fi/MQTT adapter itself is
only compiled. Pairing tests check X25519 and HKDF against RFC 7748 and RFC 5869,
tampering with every offer byte, a complete exchange between the node and receiver state
machines followed by accepted DATA, rotation, window expiry, lost JOIN_DONE and a
foreign network. The ESP32 backends (mbedTLS X25519, HKDF composed from mbedTLS HMAC)
were checked against the same vectors with a separate probe program on a board. Running
this Unity suite on the board is not part of validation. Setup tests cover button
timing, the Wi-Fi QR code, field staging, HTML escaping, session tokens and limits, host
and origin checks, notice codes and oversized pages; the access point, DNS, HTTP server,
scanning and mDNS discovery run only on hardware. Firmware tests cover signed updates:
installation in any chunking, every signed byte, header shape, role, downgrade, size,
truncation and sink failures, against a fixture signed by `tools/package_firmware.py`
with a throwaway key; the OTA adapter is only compiled. Service-safety tests inject
clock, lock, idle-state and task-startup ports. They cover immediate access, ACK
completion, timeout, lock contention, deadline boundaries, clock rollover, lock
release, stopped-state refusal, task-creation failure and watchdog-registration failure
before task release. Save-retry tests cover bounded busy retries, cancellation on
storage/STOP/apply failures or new configuration, rollover, and distinct page messages.
FreeRTOS scheduling and ESP-IDF watchdog behavior still require device validation.
Device tests cover boot-mode selection and the fault retry delay; the Arduino entry
points that use them are only compiled. Compiler/library allocation failures are not
all induced. Neither a high coverage percentage nor a passing ESP32 build proves security, radio performance,
durable flash behavior or battery life.

The ESP32 target compiles the same tests with mbedTLS and explicit
`UNITY_SUPPORT_64` for the protocol counters and identities. Build-only success is not a
physical test result. CI does not connect to devices or upload firmware.

Python unittest tests exercise provisioning, packaging, release eligibility and layout
validation. With `--coverage`, each file in `PYTHON_FLOORS` must independently reach
95% lines and branches, without rounding. The policy covers the client, packager,
partition parser, release assembler, publisher and CI gate. Tests include moved/unrelated tags,
failed or incomplete CI, partial reruns without masking newer failures, paginated
results, numeric release ordering, same-run draft recovery, interrupted uploads,
remote asset corruption, modified layouts, compiled-table mismatches, incorrect slot
sizes, and signatures made with disposable keys. A regression test checks required
job IDs against the CI workflow and rejects custom job names or matrices until the
gate supports them. Publication tests simulate GitHub; they do not create releases.

Use `uv run --python 3.12 --with coverage==7.16.2 python scripts/check_protocol.py --coverage`
or install coverage.py 7.16.2 in a virtual environment. Covered and ordinary tests use the
same interpreter. CI tests the minimum Python 3.10 separately; native coverage uses 3.12.
`--python-only` runs Python tests without compiling C++.

After building `runtime_tx` and `runtime_rx`, CI also runs
`python -m unittest discover -s tests_release -v` with the release requirements installed.
This exercises the real esptool merge, generated manifests and storage erase image, and
signs/verifies both compiled application images with a disposable key. It does not use
the release secret, publish artifacts, exercise GitHub environment protection, or upload
a device. The Stick Lite target is compiled but remains excluded from release assets.

Controller tests use a simulated clock, radio and jitter source, including time
rollover, completion timestamps, cancellation, driver failures and invalid ACKs.
A fixed pre-refactor wire fixture checks compatibility in addition to round trips.
Coverage includes the send and receive controllers and measurement normalization.
It excludes the board application, SX1262 adapter, FreeRTOS scheduling and sensor
hardware adapter. Those need physical tests; compile success is not timing validation.

## Board lint and fuzzing

`python3 scripts/lint_board.py` runs the same clang-tidy checks over `src/`, which only
compiles against the Arduino-ESP32 and ESP-IDF headers: it takes each image's compile
database from PlatformIO, swaps the Xtensa GCC for clang with the toolchain's include
directories, and reports only diagnostics in `src/`. CI runs it after the ESP32 build.

`bash scripts/fuzz.sh [seconds]` builds two libFuzzer targets with ASan and UBSan and runs each
for the given time (default 60 s): `test/fuzz/fuzz_frames.cpp` feeds untrusted bytes to the
radio frame parsing that runs before authentication (`untrustedType`, `untrustedDataNode`,
the header and length checks of `open`, and the pairing parsers; the decrypted payload is
never reached, since the fuzzer cannot forge a GCM tag), and `test/fuzz/fuzz_commands.cpp` to
the MQTT command parser and topic check. Versioned synthetic seeds in `test/fuzz/seeds/`
exercise valid command grammar, topic shapes, pairing headers and malformed inputs.
Radio seeds with placeholder tags cover parsing, not authenticated payloads.
Each run copies these into `.pio/fuzz/corpus_*`; local runs retain newly discovered inputs.
CI starts from the versioned seeds. Failure artifacts (including crash, timeout, OOM and
leak inputs) go to `.pio/fuzz/artifacts/` and are uploaded on failure.
It needs a clang with libFuzzer (Apple's has none) and OpenSSL; CI uses clang++-18.

## Tool and coverage policy

`--lint` runs ShellCheck 0.10.0 and actionlint 1.7.7 through pinned PyPI wrappers,
using `uvx` locally and hash-locked requirements in CI. ShellCheck is also available
to actionlint for checking embedded workflow scripts. All lint tools follow the
same installation path; separate Homebrew packages are not required.

Every `lib/*/src/*.cpp` must be gated or explicitly excluded with a reason. The sole
current exclusion is the ESP-IDF NVS adapter. Removed policy entries also fail the
check. Zero branch counts fail unless the file has been reviewed and explicitly declared
branchless; no files currently need that exception. This distinguishes legitimate
straight-line code from silently missing instrumentation without guessing from C++ text.

Linux CI selects Clang/LLVM 18 explicitly for compilation, profile merging and coverage
export. Distribution patch updates are still allowed; this is not a bit-reproducible
toolchain lock. Local overrides are `CC`, `CXX`, `LLVM_PROFDATA` and `LLVM_COV`; use
matching LLVM tools. macOS defaults use the corresponding Xcode tools through `xcrun`.
