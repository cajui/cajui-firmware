# CI requirements

Hash-locked tool versions for the CI jobs (Linux x86-64, Python 3.12). Edit the `.in`
file, then regenerate its lock file:

```sh
uv pip compile --generate-hashes --python-version 3.12 \
  --python-platform x86_64-unknown-linux-gnu --no-header \
  --output-file .github/requirements/lint.txt .github/requirements/lint.in
```

Keep the versions in `scripts/check_protocol.py` (`TOOLS`) in step with `lint.in` and
`native.in`.
