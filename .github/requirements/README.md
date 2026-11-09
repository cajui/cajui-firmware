# CI requirements

Hash-locked tool versions for Linux x86-64 CI jobs. The main jobs use Python 3.12;
the compatibility job uses Python 3.10. Edit the `.in` file, then regenerate its lock
file using the target Python version:

```sh
uv pip compile --generate-hashes --python-version 3.12 \
  --python-platform x86_64-unknown-linux-gnu --no-header \
  --output-file .github/requirements/lint.txt .github/requirements/lint.in
```

Keep `TOOLS`, `COVERAGE_VERSION` and `COVERAGE_PYTHON_310_VERSION` in
`scripts/check_protocol.py` in step with `lint.in`, `native.in` and `python.in`.
Native coverage uses 7.17.0 on Python 3.12; the runner also selects it on Python 3.11
or newer. The minimal `python.txt` lock keeps coverage 7.16.2 for the Python 3.10
compatibility job without installing PlatformIO, since coverage 7.17.0 requires
Python 3.11 or newer.

Regenerate these locks from the repository root:

```sh
uv pip compile --generate-hashes --python-version 3.12 \
  --python-platform x86_64-unknown-linux-gnu --no-header \
  --output-file .github/requirements/native.txt .github/requirements/native.in
uv pip compile --directory .github/requirements --generate-hashes --python-version 3.10 \
  --python-platform x86_64-unknown-linux-gnu --no-header \
  --output-file python.txt python.in
```

Keep existing output files so uv retains unrelated pins; do not use a blanket
`--upgrade`. Use `--upgrade-package coverage==7.17.0` to refresh only coverage and
its hashes. If recovering a lock that omitted PlatformIO's pip dependency, use
`--upgrade-package pip==26.2.1` to restore its existing version, not the latest one.
No `--emit-index-url` or `--emit-index-annotation` flag is needed for these locks.
