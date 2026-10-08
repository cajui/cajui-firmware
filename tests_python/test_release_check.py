# SPDX-License-Identifier: Apache-2.0
import os
import io
import re
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
            release,
            "api",
            side_effect=lambda path: [[]] if "/releases?" in path else self.fail(path),
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
                dict(name=name, status="completed", conclusion="success", run_attempt=2, id=i)
                for i, name in enumerate(release.REQUIRED_JOBS, 1)
            ]

            def api(path):
                if "/releases?" in path:
                    return [[]]
                if "/jobs?filter=all" in path:
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

    def test_versions_are_numeric_and_canonical(self):
        self.assertGreater(release.version("v1.10.0"), release.version("v1.9.99"))
        for tag in ("v0.0.0", "v01.0.0", "v100.0.0", "v1.0.0-rc1", "1.0.0"):
            with self.subTest(tag=tag), self.assertRaises(release.ReleaseError):
                release.version(tag)

    def test_publication_policy_and_same_run_recovery(self):
        sha = "a" * 40
        marker = release.publication_marker("42", sha)
        current = dict(tag_name="v1.2.3", draft=False, prerelease=False, body=marker)
        records = [dict(current, tag_name="v1.2.2", body="older")]
        with patch.object(release, "api", return_value=[records, []]):
            self.assertIsNone(release.release_state("v1.2.3", "a/b", sha))
        with patch.object(release, "api", return_value=[[current]]):
            with self.assertRaisesRegex(release.ReleaseError, "already exists"):
                release.release_state("v1.2.3", "a/b", sha)
            for run_id, revision in (("43", sha), ("42", "b" * 40)):
                with self.assertRaises(release.ReleaseError):
                    release.release_state("v1.2.3", "a/b", revision, run_id)
            self.assertEqual(current, release.release_state("v1.2.3", "a/b", sha, "42"))
        for changes in ({"draft": True}, {"body": "Notes\n" + marker + "\n"}):
            record = dict(current, **changes)
            with patch.object(release, "api", return_value=[[record]]):
                self.assertEqual(record, release.release_state("v1.2.3", "a/b", sha, "42"))
        for changes in ({"body": None}, {"body": "prefix " + marker}, {"prerelease": True}):
            with patch.object(release, "api", return_value=[[dict(current, **changes)]]):
                with self.assertRaises(release.ReleaseError):
                    release.release_state("v1.2.3", "a/b", sha, "42")
        for changes in ({}, {"tag_name": "v1.10.0"}, {"tag_name": "unexpected"}):
            record = dict(current, tag_name="v1.3.0")
            record.update(changes)
            with patch.object(release, "api", return_value=[[current], [record]]):
                with self.assertRaises(release.ReleaseError):
                    release.release_state("v1.2.3", "a/b", sha, "42")
        for flag in ("draft", "prerelease"):
            with patch.object(
                release, "api", return_value=[[dict(current, tag_name="v2.0.0", **{flag: True})]]
            ):
                self.assertIsNone(release.release_state("v1.2.3", "a/b", sha))
        for run_id, revision in (("0", sha), ("1", "bad")):
            with self.assertRaises(release.ReleaseError):
                release.publication_marker(run_id, revision)

    def test_git_and_api_failures_are_readable(self):
        error = subprocess.CalledProcessError(1, ["git"], stderr="missing ref")
        with patch.object(release.subprocess, "check_output", side_effect=error):
            with self.assertRaisesRegex(release.ReleaseError, "missing ref"):
                release.git("show", "missing:file")
            with self.assertRaisesRegex(release.ReleaseError, "missing ref"):
                release.api("repos/a/b/releases")

    def test_cli_reports_validation_errors_without_traceback(self):
        args = [
            "release_check",
            "--tag",
            "v1.2.3",
            "--repository",
            "a/b",
            "--trusted-sha",
            "a" * 40,
        ]
        with patch.object(sys, "argv", args), patch.object(sys, "stderr", io.StringIO()) as stderr:
            with patch.object(release, "check", side_effect=release.ReleaseError("Invalid tag")):
                with self.assertRaises(SystemExit) as error:
                    release.main()
        self.assertEqual(1, error.exception.code)
        self.assertIn("Release validation failed: Invalid tag", stderr.getvalue())

    def test_required_jobs_match_ci_without_name_overrides_or_matrix(self):
        workflow = (Path(__file__).resolve().parents[1] / ".github/workflows/ci.yml").read_text()
        job_ids = set(re.findall(r"^  ([A-Za-z_][A-Za-z0-9_-]*):$", workflow, re.MULTILINE))
        self.assertEqual(release.REQUIRED_JOBS, job_ids)
        self.assertNotRegex(workflow, r"(?m)^    (?:name|strategy):")

    def test_partial_rerun_keeps_successes_but_never_masks_a_newer_failure(self):
        original = [
            dict(name=name, status="completed", conclusion="success", id=i, run_attempt=1)
            for i, name in enumerate(sorted(release.REQUIRED_JOBS), 1)
        ]
        original[0]["conclusion"] = "failure"
        retry = dict(original[0], id=100, run_attempt=2, conclusion="success")

        def responses(path):
            if "/jobs?filter=all" in path:
                return [{"jobs": [retry]}, {"jobs": original}]
            return [{"workflow_runs": [self.run_record()]}]

        with patch.object(release, "api", side_effect=responses):
            release.require_ci("a/b", "a" * 40)
            original[0]["conclusion"] = "success"
            for result in ("failure", "skipped", "cancelled"):
                retry["conclusion"] = result
                with self.subTest(result=result), self.assertRaises(release.ReleaseError):
                    release.require_ci("a/b", "a" * 40)
            retry["conclusion"] = "success"
            retry["run_attempt"] = 3
            with self.assertRaisesRegex(release.ReleaseError, "attempt changed"):
                release.require_ci("a/b", "a" * 40)
