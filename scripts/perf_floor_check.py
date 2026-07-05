#!/usr/bin/env python3
"""
perf_floor_check.py - synthetic-benchmark throughput floor check.

Wraps `aster_sim --events N`, parses its `Throughput:` line, and asserts
that the measured M-events/sec is >= `--floor`. Same logic as the inline
awk block in .github/workflows/ci.yml::perf-floor::Run throughput floor
check; both callers should agree so local / CI results are directly
comparable.

This script is the canonical implementation of the floor check. The
`perf-floor` CMake custom target delegates to it so the floor logic
lives in exactly one place and can't drift between local and CI.

Usage:
  python3 scripts/perf_floor_check.py \\
      --binary build/aster_sim \\
      --events 1000000 \\
      --floor 10.0

Exit codes:
  0  throughput >= floor
  1  throughput below floor (or aster_sim non-zero exit, or parse error)
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time


# Match the `Throughput: <float> M events/sec` line that aster_sim emits
# in single-run mode (see src/sim.cpp). Float-only, anchored on the
# exact suffix so unrelated lines can't accidentally pass.
RE_THROUGHPUT = re.compile(
    r"^\s*Throughput:\s*(?P<m>[0-9]+(?:\.[0-9]+)?)\s*M events/sec\s*$",
    re.MULTILINE,
)


def parse_throughput(stdout: str) -> float:
    """Extract the first M-events/sec number from aster_sim's stdout.

    Returns NaN if no `Throughput:` line is present.
    """
    m = RE_THROUGHPUT.search(stdout)
    if not m:
        return float("nan")
    return float(m.group("m"))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Assert aster_sim's synthetic-benchmark throughput floor."
    )
    parser.add_argument(
        "--binary",
        required=True,
        help="Path to the aster_sim binary (e.g. build/aster_sim).",
    )
    parser.add_argument(
        "--events",
        type=int,
        required=True,
        help="Number of events to dispatch (passed verbatim to aster_sim).",
    )
    parser.add_argument(
        "--floor",
        type=float,
        required=True,
        help="Floor in M events/sec (e.g. 10.0).",
    )
    args = parser.parse_args(argv)

    if args.events <= 0:
        sys.stderr.write("ERROR: --events must be > 0\n")
        return 1
    if args.floor <= 0:
        sys.stderr.write("ERROR: --floor must be > 0\n")
        return 1

    start = time.monotonic()
    proc = subprocess.run(
        [args.binary, "--events", str(args.events)],
        capture_output=True,
        text=True,
    )
    wall_s = time.monotonic() - start

    # Echo the binary's stdout verbatim so callers can grep for the
    # `Throughput:` line in their logs (the perf-floor CI job does this).
    sys.stdout.write(proc.stdout)
    if proc.stderr:
        sys.stderr.write(proc.stderr)

    if proc.returncode != 0:
        sys.stderr.write(
            f"ERROR: {args.binary} exited with code {proc.returncode}\n"
        )
        return 1

    throughput = parse_throughput(proc.stdout)
    if throughput != throughput:  # NaN check (NaN != NaN)
        sys.stderr.write(
            "ERROR: failed to parse 'Throughput: <float> M events/sec' "
            "line from stdout\n"
        )
        return 1

    sys.stderr.write(
        f"==> wall-clock: {wall_s:.2f}s; "
        f"measured: {throughput:.2f} M events/s; "
        f"floor: {args.floor:.2f} M events/s\n"
    )
    if throughput < args.floor:
        sys.stderr.write(
            f"==> FAIL: throughput {throughput:.2f} < floor {args.floor:.2f}\n"
        )
        return 1

    sys.stderr.write(
        f"==> OK: throughput {throughput:.2f} >= floor {args.floor:.2f}\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
