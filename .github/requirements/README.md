# CI requirements

Hash-locked tool versions for Linux x86-64 CI jobs. The main jobs use Python 3.12;
the compatibility job uses Python 3.10. Edit the `.in` file, then regenerate its lock
file using the target Python version:

```sh
uv pip compile --generate-hashes --python-version 3.12 \
  --python-platform x86_64-unknown-linux-gnu --no-header \
  --output-file .github/requirements/lint.txt .github/requirements/lint.in
```

Keep the versions in `scripts/check_protocol.py` (`TOOLS` and `COVERAGE_VERSION`) in step with `lint.in`,
`native.in` and `python.in`. The minimal `python.txt` lock supports the Python 3.10
compatibility job without installing PlatformIO.

Use `--python-version 3.10` when regenerating `python.txt` from `python.in`.
