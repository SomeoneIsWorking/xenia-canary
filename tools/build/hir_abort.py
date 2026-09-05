#!/usr/bin/env python3
"""Require an existing hidden HIR Catch case to abort, not merely fail a test."""

import argparse
import os
import signal
import subprocess
from pathlib import Path


def _run_case(executable: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    # The caller validates the local suite path; argv is explicit, never a shell.
    return subprocess.run(  # noqa: S603 - owned test-executable process boundary
        [str(executable), *arguments],
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )


def verify_abort(executable: Path, case: str) -> None:
    """Exercise the shipping suite, refusing absent cases and normal exits."""
    executable = executable.resolve(strict=True)
    if not executable.is_file():
        raise ValueError(f"The HIR test executable is not a file: {executable}")
    listing = _run_case(executable, case, "--list-test-names-only")
    # Xenia's console entry may append a final newline during logger shutdown.
    if listing.returncode != 1 or listing.stdout.rstrip("\r\n").splitlines() != [case]:
        raise RuntimeError(
            f"Expected exactly one case {case!r}; discovery returned "
            f"{listing.returncode}: {listing.stdout!r} {listing.stderr!r}"
        )
    result = _run_case(executable, case, "--warn", "NoTests")
    # With CRT reporting disabled, Windows abort terminates with status 3.
    # Catch ordinary failure for this single case is 1, never 3.
    expected = 3 if os.name == "nt" else -signal.SIGABRT
    if result.returncode != expected:
        raise RuntimeError(
            f"{case!r} did not abort: status {result.returncode}, expected "
            f"{expected}; stdout={result.stdout!r}; stderr={result.stderr!r}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("case")
    args = parser.parse_args()
    verify_abort(args.executable, args.case)
    print(f"Verified abort: {args.case}")


if __name__ == "__main__":
    main()
