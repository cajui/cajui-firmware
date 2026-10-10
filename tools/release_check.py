# SPDX-License-Identifier: Apache-2.0
"""Resolve a release tag and require successful CI on that exact main-branch commit."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

REQUIRED_JOBS = frozenset(("lint", "native", "python-minimum", "esp32-build", "fuzz"))


class ReleaseError(ValueError):
    pass


def git(*args):
    try:
        return subprocess.check_output(["git", *args], text=True, stderr=subprocess.PIPE).strip()
    except subprocess.CalledProcessError as error:
        raise ReleaseError(f"Cannot read release Git metadata: {error.stderr.strip()}") from None


def api(path):
    try:
        return json.loads(
            subprocess.check_output(
                ["gh", "api", "--paginate", "--slurp", path], text=True, stderr=subprocess.PIPE
            )
        )
    except subprocess.CalledProcessError as error:
        raise ReleaseError(f"Cannot query release metadata: {error.stderr.strip()}") from None


def version(tag):
    if not re.fullmatch(r"v(?:0|[1-9][0-9]?)\.(?:0|[1-9][0-9]?)\.(?:0|[1-9][0-9]?)", tag):
        raise ReleaseError("Expected a canonical vX.Y.Z release tag")
    result = tuple(map(int, tag[1:].split(".")))
    if result == (0, 0, 0):
        raise ReleaseError("Version zero is reserved for local builds")
    return result


def publication_marker(run_id, sha):
    if not re.fullmatch(r"[1-9][0-9]*", run_id) or not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ReleaseError("Invalid publication identity")
    return f"<!-- cajui-release run={run_id} sha={sha} -->"


def release_state(tag, repository, sha, publication_run=None):
    requested = version(tag)
    marker = publication_marker(publication_run, sha) if publication_run else None
    existing = None
    for page in api(f"repos/{repository}/releases?per_page=100"):
        for record in page:
            if record["tag_name"] == tag:
                if marker is None or marker not in (record.get("body") or "").splitlines():
                    raise ReleaseError("Release already exists; resume its original publish job")
                if record["prerelease"]:
                    raise ReleaseError("Cannot resume a prerelease as a stable release")
                existing = record
            elif not record["draft"] and not record["prerelease"]:
                if version(record["tag_name"]) >= requested:
                    raise ReleaseError("Release must be newer than every published stable version")
    return existing


def approved_run(runs, sha):
    matching = [
        run
        for run in runs
        if run.get("head_sha") == sha
        and run.get("event") == "push"
        and run.get("head_branch") == "main"
        and run.get("path") == ".github/workflows/ci.yml"
    ]
    if not matching:
        raise ReleaseError("No main-branch CI run exists for the release commit")
    latest = max(matching, key=lambda run: run["id"])
    if latest.get("status") != "completed" or latest.get("conclusion") != "success":
        raise ReleaseError("The latest CI run must complete successfully before releasing")
    return latest


def check_jobs(jobs):
    passed = {
        job["name"]
        for job in jobs
        if job.get("status") == "completed" and job.get("conclusion") == "success"
    }
    if not REQUIRED_JOBS <= passed or any(job.get("conclusion") != "success" for job in jobs):
        raise ReleaseError("Required CI jobs are missing, skipped or unsuccessful")


def check(tag, repository, trusted_sha, expected_sha=None, publication_run=None):
    version(tag)
    if not re.fullmatch(
        r"[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*", repository
    ) or not re.fullmatch(r"[0-9a-f]{40}", trusted_sha):
        raise ReleaseError("Invalid repository or trusted revision")
    sha = git("rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}")
    if expected_sha and sha != expected_sha:
        raise ReleaseError("Release tag changed after validation")
    result = subprocess.run(["git", "merge-base", "--is-ancestor", sha, trusted_sha], check=False)
    if result.returncode:
        raise ReleaseError("Release commit must belong to the trusted main history")
    for path in ("partitions.csv", "src/board/release_key.h"):
        if git("show", f"{sha}:{path}") != git("show", f"{trusted_sha}:{path}"):
            raise ReleaseError(f"Release {path} differs from the trusted revision")
    for revision in dict.fromkeys((sha, trusted_sha)):
        require_ci(repository, revision)
    release_state(tag, repository, sha, publication_run)
    return sha


def require_ci(repository, sha):
    runs = [
        run
        for page in api(
            f"repos/{repository}/actions/workflows/ci.yml/runs?head_sha={sha}&event=push&per_page=100"
        )
        for run in page["workflow_runs"]
    ]
    run = approved_run(runs, sha)
    jobs = [
        job
        for page in api(f"repos/{repository}/actions/runs/{run['id']}/jobs?filter=all&per_page=100")
        for job in page["jobs"]
    ]
    latest = {}
    for job in sorted(jobs, key=lambda job: (job["run_attempt"], job["id"])):
        if not 1 <= job["run_attempt"] <= run["run_attempt"]:
            raise ReleaseError("CI attempt changed during validation; retry after CI completes")
        latest[job["name"]] = job
    check_jobs(list(latest.values()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--trusted-sha", required=True)
    parser.add_argument("--expected-sha")
    parser.add_argument("--publication-run")
    args = parser.parse_args()
    try:
        sha = check(
            args.tag, args.repository, args.trusted_sha, args.expected_sha, args.publication_run
        )
    except (ReleaseError, OSError) as error:
        print(f"Release validation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from None
    with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
        output.write(f"sha={sha}\nversion={args.tag[1:]}\n")


if __name__ == "__main__":
    main()
