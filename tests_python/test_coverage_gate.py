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
