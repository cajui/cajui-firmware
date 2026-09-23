# Development of Cajuí Firmware

- Keep source comments, documentation and commit messages in English.
- Read README.md and docs/protocol-v1.md before modifying wire behavior.
- Keep protocol logic independent of radio drivers, sensors and storage adapters.
- Never implement cryptographic primitives locally; use established libraries.
- Preserve nonce uniqueness, authenticated context, replay state and commit-before-ACK semantics.
- Test behavior and failures, including restarts. Coverage does not replace review.
- Run `python3 scripts/check_protocol.py --lint`, then `--coverage` with Clang/LLVM, and
  compile the ESP32 test target with `--without-uploading --without-testing` before submitting.
- Keep formatting-only changes in their own commit and list it in `.git-blame-ignore-revs`.
- Do not imply persistence, radio timing or field reliability is implemented when
  only a contract or test double exists. Document integration limitations.
- Never commit credentials, device-specific configuration or local machine paths.
- Do not upload firmware or access physical devices as part of automated checks.
