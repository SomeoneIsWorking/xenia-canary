"""Discriminators for the narrow HIR fatal-case subprocess contract."""

import importlib.util
import signal
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "hir_abort", Path(__file__).parents[1] / "hir_abort.py"
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("Cannot load the production HIR abort harness")
HIR_ABORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HIR_ABORT)


class AbortHarnessTests(unittest.TestCase):
    def run_probe(self, status, platform="posix", listing="case\n", count=1):
        discovered = subprocess.CompletedProcess([], count, listing, "")
        result = subprocess.CompletedProcess([], status, "", "")
        executable = Path(__file__).resolve()
        with (
            patch.object(HIR_ABORT.os, "name", platform),
            patch.object(type(executable), "resolve", return_value=executable),
            patch.object(HIR_ABORT.subprocess, "run", side_effect=[discovered, result]) as run,
        ):
            HIR_ABORT.verify_abort(executable, "case")
            self.assertEqual(run.call_count, 2)
            for call in run.call_args_list:
                self.assertEqual(call.kwargs["timeout"], 10)

    def test_abort_is_accepted(self):
        self.run_probe(-signal.SIGABRT)
        self.run_probe(3, platform="nt")
        self.run_probe(-signal.SIGABRT, listing="case\n\n")
        self.run_probe(3, platform="nt", listing="case\r\n\r\n")

    def test_normal_exit_or_wrong_signal_is_rejected(self):
        for status in (0, 1, 2, 3, -signal.SIGSEGV):
            with self.subTest(status=status), self.assertRaises(RuntimeError):
                self.run_probe(status)
        for status in (0, 1, 2):
            with self.subTest(windows=status), self.assertRaises(RuntimeError):
                self.run_probe(status, platform="nt")

    def test_missing_or_ambiguous_case_is_rejected(self):
        for listing, count in (
            ("", 0),
            ("other\n", 1),
            ("case\nother\n", 2),
            ("\ncase\n", 1),
            ("case\n\nother\n", 1),
        ):
            with self.subTest(listing=listing), self.assertRaises(RuntimeError):
                self.run_probe(-signal.SIGABRT, listing=listing, count=count)

    def test_timeout_is_not_a_success(self):
        discovered = subprocess.CompletedProcess([], 1, "case\n", "")
        for responses in (
            [subprocess.TimeoutExpired("suite", 10)],
            [discovered, subprocess.TimeoutExpired("suite", 10)],
        ):
            with patch.object(HIR_ABORT.subprocess, "run", side_effect=responses):
                with self.assertRaises(subprocess.TimeoutExpired):
                    HIR_ABORT.verify_abort(Path(__file__), "case")

    def test_missing_executable_is_rejected_before_discovery(self):
        with patch.object(HIR_ABORT.subprocess, "run") as run:
            with self.assertRaises(FileNotFoundError):
                HIR_ABORT.verify_abort(Path(__file__).parent / "missing", "case")
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
