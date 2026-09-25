"""Regression tests for the client coverage gate, independent of the coverage tool."""

import contextlib
import importlib.util
import io
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "check_protocol", Path(__file__).resolve().parents[1] / "scripts/check_protocol.py"
)
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)


def report(lines, statements, branches, possible):
    return {
        "files": {
            "tools/provision.py": {
                "summary": {
                    "covered_lines": lines,
                    "num_statements": statements,
                    "covered_branches": branches,
                    "num_branches": possible,
                }
            }
        }
    }


class CoverageGateTests(unittest.TestCase):
    def test_combined_percentage_cannot_hide_a_failing_metric(self):
        for label, counts in (
            ("branches", (222, 228, 88, 94)),  # Original regression: combined 96.27%.
            ("lines", (94, 100, 1000, 1000)),
        ):
            with self.subTest(label=label), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(SystemExit, label):
                    checks.check_python_coverage(report(*counts))

    def test_minimum_is_inclusive_without_rounding_up(self):
        with contextlib.redirect_stdout(io.StringIO()):
            checks.check_python_coverage(report(95, 100, 95, 100))
            for counts in ((9499, 10000, 100, 100), (100, 100, 9499, 10000)):
                with self.subTest(counts=counts), self.assertRaises(SystemExit):
                    checks.check_python_coverage(report(*counts))

    def test_missing_or_empty_metrics_fail_closed(self):
        for data in ({}, {"files": {}}, report(0, 0, 1, 1), report(1, 1, 0, 0)):
            with self.subTest(data=data), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit):
                    checks.check_python_coverage(data)


def native(files):
    return {
        "data": [
            {
                "files": [
                    {
                        "filename": "/work/repo/" + name,
                        "summary": {
                            "lines": {"count": 100, "percent": lines},
                            "branches": {
                                "count": 100 if branches is not None else 0,
                                "percent": branches or 0.0,
                            },
                        },
                    }
                    for name, lines, branches in files
                ]
            }
        ]
    }


class NativeCoverageGateTests(unittest.TestCase):
    def test_each_file_must_pass_on_its_own(self):
        report = native([("lib/A/src/a.cpp", 100.0, 99.0), ("lib/B/src/b.cpp", 99.0, 84.9)])
        failures = checks.check_native_coverage(report, ("lib/A/src/a.cpp", "lib/B/src/b.cpp"))
        self.assertEqual(["lib/B/src/b.cpp: branches 84.90% < 85%"], failures)
        report = native([("lib/B/src/b.cpp", 94.99, 90.0)])
        self.assertIn("lines", checks.check_native_coverage(report, ("lib/B/src/b.cpp",))[0])

    def test_documented_floor_and_files_without_branches(self):
        crypto = "lib/CajuiProtocol/src/crypto.cpp"
        self.assertEqual(
            [], checks.check_native_coverage(native([(crypto, 97.0, 60.0)]), (crypto,))
        )
        self.assertEqual(
            1, len(checks.check_native_coverage(native([(crypto, 97.0, 59.9)]), (crypto,)))
        )
        self.assertEqual(
            [], checks.check_native_coverage(native([("lib/C/src/c.cpp", 100.0, None)]), ())
        )

    def test_missing_files_and_malformed_reports_fail_closed(self):
        self.assertEqual(
            ["lib/A/src/a.cpp: no coverage data"],
            checks.check_native_coverage(native([]), ("lib/A/src/a.cpp",)),
        )
        for data in ({}, {"data": []}, None):
            with self.subTest(data=data):
                self.assertEqual(1, len(checks.check_native_coverage(data, ())))
        self.assertTrue(set(checks.BRANCH_FLOORS) <= set(checks.GATED_FILES))
