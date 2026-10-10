# SPDX-License-Identifier: Apache-2.0
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from tools import release_publish as publisher
from tools.release_check import ReleaseError


class ReleasePublishTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.files = [self.root / "SHA256SUMS"] + [
            self.root / f"cajui-{role}-1.2.3{suffix}"
            for role in ("receiver", "transmitter")
            for suffix in (".cjfw", "-full.bin")
        ]
        for path in self.files:
            path.write_bytes(path.name.encode())
        self.record = dict(
            draft=True, assets=[dict(name=p.name, size=p.stat().st_size) for p in self.files]
        )
        self.commands = []
        self.fail_upload = False
        self.corrupt = False

    def gh(self, repository, *args):
        self.assertEqual("a/b", repository)
        self.commands.append(args)
        if args[0] == "download":
            folder = Path(args[-1])
            for path in self.files:
                shutil.copyfile(path, folder / path.name)
            if self.corrupt:
                (folder / "SHA256SUMS").write_text("corrupt")
        if args[0] == "upload" and self.fail_upload:
            raise subprocess.CalledProcessError(1, ["gh", "release", "upload"])

    def publish(self, states):
        with (
            patch.object(publisher, "release_state", side_effect=states),
            patch.object(publisher, "gh", side_effect=self.gh),
        ):
            publisher.publish("v1.2.3", "a/b", "a" * 40, "42", self.root)

    def test_fresh_release_is_verified_before_publishing(self):
        self.publish([None, self.record])
        self.assertEqual(["create", "upload", "download", "edit"], [c[0] for c in self.commands])
        self.assertIn("--draft", self.commands[0])
        self.assertIn("--clobber", self.commands[1])
        self.assertIn("--latest", self.commands[-1])

    def test_partial_upload_remains_draft_and_can_resume(self):
        self.fail_upload = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.publish([None])
        self.assertEqual(["create", "upload"], [c[0] for c in self.commands])
        self.commands.clear()
        self.fail_upload = False
        self.publish([self.record, self.record])
        self.assertEqual(["upload", "download", "edit"], [c[0] for c in self.commands])

    def test_published_release_is_only_verified_never_overwritten(self):
        published = dict(self.record, draft=False)
        self.publish([published, published])
        self.assertEqual(["download"], [c[0] for c in self.commands])

    def test_different_version_or_execution_cannot_mutate_release(self):
        with (
            patch.object(publisher, "release_state", side_effect=ReleaseError("Not eligible")),
            patch.object(publisher, "gh") as gh,
        ):
            with self.assertRaises(ReleaseError):
                publisher.publish("v1.2.3", "a/b", "a" * 40, "42", self.root)
            gh.assert_not_called()

    def test_mismatched_remote_assets_are_not_published(self):
        for assets in (
            [],
            self.record["assets"] + [self.record["assets"][0]],
            [dict(a, size=0) for a in self.record["assets"]],
        ):
            with (
                self.subTest(assets=assets),
                self.assertRaisesRegex(ReleaseError, "names and sizes"),
            ):
                self.publish([self.record, dict(self.record, assets=assets)])
        self.corrupt = True
        with self.assertRaisesRegex(ReleaseError, "checksum"):
            self.publish([self.record, self.record])
        self.assertNotIn("edit", [c[0] for c in self.commands])

    def test_missing_remote_release_stops_publication(self):
        with self.assertRaisesRegex(ReleaseError, "disappeared"):
            self.publish([None, None])

    def test_local_assets_must_be_exact_regular_nonempty_files(self):
        path = self.files[0]
        for kind in ("missing", "empty", "directory", "symlink"):
            with self.subTest(kind=kind):
                path.unlink()
                if kind == "empty":
                    path.touch()
                elif kind == "directory":
                    path.mkdir()
                elif kind == "symlink":
                    path.symlink_to(self.files[1])
                with self.assertRaisesRegex(ReleaseError, "five expected"):
                    self.publish([None])
                if path.is_dir():
                    path.rmdir()
                elif path.exists():
                    path.unlink()
                path.write_text("checksum")
        self.assertEqual([], self.commands)

    def test_cli_and_subprocess_arguments(self):
        with patch.object(publisher.subprocess, "run") as run:
            publisher.gh("a/b", "edit", "v1.2.3", "--draft=false")
        run.assert_called_once_with(
            ["gh", "release", "edit", "v1.2.3", "--draft=false", "--repo", "a/b"], check=True
        )
        args = [
            "publish",
            "--tag",
            "v1.2.3",
            "--repository",
            "a/b",
            "--sha",
            "a" * 40,
            "--run-id",
            "42",
            "--directory",
            str(self.root),
        ]
        with patch.object(sys, "argv", args), patch.object(publisher, "publish") as publish:
            publisher.main()
            publish.assert_called_once_with("v1.2.3", "a/b", "a" * 40, "42", self.root)
        with (
            patch.object(sys, "argv", args),
            patch.object(publisher, "publish", side_effect=ReleaseError("Stopped")),
            patch.object(sys, "stderr", io.StringIO()) as stderr,
        ):
            with self.assertRaises(SystemExit) as error:
                publisher.main()
        self.assertEqual(1, error.exception.code)
        self.assertIn("Release publication failed: Stopped", stderr.getvalue())
