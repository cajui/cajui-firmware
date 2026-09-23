# Contributing

Start with the README and protocol specification. Open an issue for wire-format or
security changes before implementing an incompatible design. Small bug fixes can
be submitted directly as pull requests.

Keep shared logic in C++11 and independent of the board. Use bounded buffers and
explicit serialization. Add Unity tests for behavior, malformed input and failure
paths. Test doubles must describe the guarantees required of real adapters.

Run `scripts/check_protocol.py --lint`, native tests with sanitizers and coverage, then
compile the ESP32 test target without uploading. CI runs all three checks. Include the problem, resulting behavior,
validation and remaining limitations in the pull request.

Write code comments and documentation in English. Do not include secrets, device
identifiers or environment-specific files. Contributions are under Apache-2.0.
