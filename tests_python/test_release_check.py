# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import call, patch

from tools import release_check as release


class ReleaseCheckTests(unittest.TestCase):
    def run_record(self, sha="a" * 40, **changes):
        return dict(
            id=10,
            head_sha=sha,
            event="push",
            head_branch="main",
            path=".github/workflows/ci.yml",
            status="completed",
            conclusion="success",
            run_attempt=2,
            **changes,
        )

    def test_only_latest_exact_main_ci_can_authorize_release(self):
        sha = "a" * 40
        good = self.run_record()
        self.assertEqual(good, release.approved_run([good], sha))
        for field, value in [
            ("head_sha", "b" * 40),
            ("event", "pull_request"),
            ("head_branch", "feature"),
            ("path", ".github/workflows/other.yml"),
        ]:
            bad = dict(good, **{field: value})
            with self.subTest(field=field), self.assertRaises(release.ReleaseError):
                release.approved_run([bad], sha)
        for state, result in [
            ("queued", None),
            ("in_progress", None),
            ("completed", "failure"),
            ("completed", "cancelled"),
        ]:
            newer = dict(good, id=11, status=state, conclusion=result)
            with self.subTest(state=state, result=result), self.assertRaises(release.ReleaseError):
                release.approved_run([good, newer], sha)

    def test_missing_skipped_failed_jobs_cannot_be_hidden_by_success(self):
        jobs = [
            dict(name=name, status="completed", conclusion="success")
            for name in release.REQUIRED_JOBS
        ]
        release.check_jobs(jobs)
        for bad in (
            jobs[:-1],
            jobs + [dict(name="additional", conclusion="failure")],
            [dict(j, conclusion="skipped") for j in jobs],
            [dict(j, status="in_progress") for j in jobs],
        ):
            with self.subTest(jobs=bad), self.assertRaises(release.ReleaseError):
                release.check_jobs(bad)

    def test_api_pages_are_decoded_without_shell_interpolation(self):
        with patch.object(
            release.subprocess, "check_output", return_value='[{"jobs":[]},{"jobs":[1]}]'
        ) as call:
            self.assertEqual([{"jobs": []}, {"jobs": [1]}], release.api("repos/a/b/path"))
        self.assertEqual(
            ["gh", "api", "--paginate", "--slurp", "repos/a/b/path"], call.call_args.args[0]
        )

    def test_real_git_history_tag_movement_and_untrusted_metadata(self):
        offline = patch.object(
            release, "api", side_effect=AssertionError("Unexpected network request")
        )
        offline.start()
        self.addCleanup(offline.stop)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            previous = Path.cwd()
            os.chdir(root)
            self.addCleanup(os.chdir, previous)
            subprocess.run(["git", "init", "-q"], check=True)
            subprocess.run(["git", "config", "user.name", "Release Test"], check=True)
            subprocess.run(["git", "config", "user.email", "release@example.invalid"], check=True)
            (root / "src/board").mkdir(parents=True)
            (root / "partitions.csv").write_text("layout")
            (root / "src/board/release_key.h").write_text("public key")
            subprocess.run(["git", "add", "."], check=True)
            subprocess.run(["git", "commit", "-qm", "Initial"], check=True)
            sha = release.git("rev-parse", "HEAD")
            subprocess.run(["git", "tag", "-a", "v1.2.3", "-m", "Release"], check=True)
            jobs = [
                dict(name=name, status="completed", conclusion="success")
                for name in release.REQUIRED_JOBS
            ]

            def api(path):
                if "/attempts/2/jobs?" in path:
                    return [{"jobs": jobs[:2]}, {"jobs": jobs[2:]}]
                self.assertIn("head_sha=" + sha, path)
                return [{"workflow_runs": []}, {"workflow_runs": [self.run_record(sha)]}]

            with patch.object(release, "api", side_effect=api):
                self.assertEqual(sha, release.check("v1.2.3", "example/firmware", sha))
                self.assertEqual(sha, release.check("v1.2.3", "example/firmware", sha, sha))
            (root / "notes.txt").write_text("New trusted tooling revision")
            subprocess.run(["git", "add", "."], check=True)
            subprocess.run(["git", "commit", "-qm", "Tooling change"], check=True)
            trusted = release.git("rev-parse", "HEAD")
            with patch.object(release, "require_ci") as gate:
                self.assertEqual(sha, release.check("v1.2.3", "example/firmware", trusted))
            self.assertEqual(
                [call("example/firmware", sha), call("example/firmware", trusted)],
                gate.call_args_list,
            )
            with patch.object(
                release, "require_ci", side_effect=[None, release.ReleaseError("Trusted CI failed")]
            ):
                with self.assertRaisesRegex(release.ReleaseError, "Trusted CI failed"):
                    release.check("v1.2.3", "example/firmware", trusted)
            subprocess.run(["git", "reset", "--hard", sha], check=True, stdout=subprocess.DEVNULL)
            with self.assertRaisesRegex(release.ReleaseError, "changed"):
                release.check("v1.2.3", "example/firmware", sha, "b" * 40)
            for tag, repo, trusted in [
                ("v1.2.3;false", "example/firmware", sha),
                ("v0.0.0", "example/firmware", sha),
                ("v01.2.3", "example/firmware", sha),
                ("v1.2.3", "../other", sha),
                ("v1.2.3", "example/firmware", "not-sha"),
            ]:
                with self.subTest(tag=tag, repo=repo), self.assertRaises(release.ReleaseError):
                    release.check(tag, repo, trusted)
            for path in ("partitions.csv", "src/board/release_key.h"):
                (root / path).write_text("changed")
                subprocess.run(["git", "commit", "-qam", "Change metadata"], check=True)
                newer = release.git("rev-parse", "HEAD")
                with self.assertRaisesRegex(release.ReleaseError, "differs"):
                    release.check("v1.2.3", "example/firmware", newer)
                subprocess.run(
                    ["git", "reset", "--hard", sha], check=True, stdout=subprocess.DEVNULL
                )
            subprocess.run(
                ["git", "checkout", "--orphan", "unrelated"],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            subprocess.run(["git", "commit", "-qm", "Different root"], check=True)
            with self.assertRaisesRegex(release.ReleaseError, "history"):
                release.check("v1.2.3", "example/firmware", release.git("rev-parse", "HEAD"))
            os.chdir(previous)

    def test_cli_emits_only_validated_outputs(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "outputs"
            args = [
                "release_check",
                "--tag",
                "v1.2.3",
                "--repository",
                "example/fw",
                "--trusted-sha",
                "a" * 40,
            ]
            with (
                patch.object(sys, "argv", args),
                patch.dict(os.environ, GITHUB_OUTPUT=str(output)),
                patch.object(release, "check", return_value="b" * 40),
            ):
                release.main()
            self.assertEqual("sha=" + "b" * 40 + "\nversion=1.2.3\n", output.read_text())
