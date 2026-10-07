# SPDX-License-Identifier: Apache-2.0
"""Resolve a release tag and require successful CI on that exact main-branch commit."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

REQUIRED_JOBS = frozenset(("lint", "native", "python-minimum", "esp32-build", "fuzz"))


class ReleaseError(ValueError):
    pass


def git(*args):
    return subprocess.check_output(["git", *args], text=True).strip()


def api(path):
    pages = json.loads(
        subprocess.check_output(["gh", "api", "--paginate", "--slurp", path], text=True)
    )
    return pages


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


def check(tag, repository, trusted_sha, expected_sha=None):
    if (
        not re.fullmatch(r"v(?:0|[1-9][0-9]?)\.(?:0|[1-9][0-9]?)\.(?:0|[1-9][0-9]?)", tag)
        or tag == "v0.0.0"
    ):
        raise ReleaseError("Expected a canonical vX.Y.Z release tag")
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
        for page in api(
            f"repos/{repository}/actions/runs/{run['id']}/attempts/{run['run_attempt']}/jobs?per_page=100"
        )
        for job in page["jobs"]
    ]
    check_jobs(jobs)
    return sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--trusted-sha", required=True)
    parser.add_argument("--expected-sha")
    args = parser.parse_args()
    sha = check(args.tag, args.repository, args.trusted_sha, args.expected_sha)
    with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
        output.write(f"sha={sha}\nversion={args.tag[1:]}\n")


if __name__ == "__main__":
    main()
