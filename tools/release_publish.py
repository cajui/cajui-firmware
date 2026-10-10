# SPDX-License-Identifier: Apache-2.0
"""Publish verified assets through a resumable draft owned by one workflow run."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from .release_check import ReleaseError, publication_marker, release_state


def gh(repository, *args):
    subprocess.run(["gh", "release", *args, "--repo", repository], check=True)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            result.update(block)
    return result.hexdigest()


def verify_assets(repository, tag, record, files):
    expected = {path.name: path.stat().st_size for path in files}
    actual = {asset["name"]: asset["size"] for asset in record["assets"]}
    if expected != actual or len(record["assets"]) != len(expected):
        raise ReleaseError("Remote release assets do not match the expected names and sizes")
    with tempfile.TemporaryDirectory() as folder:
        gh(repository, "download", tag, "--pattern", "*", "--dir", folder)
        for path in files:
            if digest(path) != digest(Path(folder) / path.name):
                raise ReleaseError(f"Remote release asset checksum mismatch: {path.name}")


def publish(tag, repository, sha, run_id, directory):
    record = release_state(tag, repository, sha, run_id)
    version = tag[1:]
    names = {
        f"cajui-{role}-{version}{suffix}"
        for role in ("transmitter", "receiver")
        for suffix in (".cjfw", "-full.bin")
    }
    files = sorted(directory.iterdir())
    if {path.name for path in files} != names | {"SHA256SUMS"} or any(
        path.is_symlink() or not path.is_file() or path.stat().st_size == 0 for path in files
    ):
        raise ReleaseError("Release directory must contain exactly the five expected assets")
    if record is None:
        gh(
            repository,
            "create",
            tag,
            "--draft",
            "--verify-tag",
            "--title",
            tag,
            "--generate-notes",
            "--notes",
            publication_marker(run_id, sha),
        )
    if record is None or record["draft"]:
        gh(repository, "upload", tag, *map(str, files), "--clobber")
    record = release_state(tag, repository, sha, run_id)
    if record is None:
        raise ReleaseError("Release disappeared during publication")
    verify_assets(repository, tag, record, files)
    if record["draft"]:
        gh(repository, "edit", tag, "--draft=false", "--latest")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    try:
        publish(args.tag, args.repository, args.sha, args.run_id, args.directory)
    except (ReleaseError, OSError, subprocess.CalledProcessError) as error:
        print(f"Release publication failed: {error}", file=sys.stderr)
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
