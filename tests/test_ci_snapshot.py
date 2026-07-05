#!/usr/bin/env python3
"""
test_ci_snapshot.py - offline ci/ mirror regression gate.

Loads the artifacts captured by `scripts/capture_ci.sh`:
  * ci/last_run_jobs.json   list of per-job records for the most-recent
                            GH Actions run on `main` (each record has
                            `name`, `conclusion`, ...)
  * ci/badge.svg            the README/badge SVG (its <title> element
                            tracks overall CI health: "CI - passing",
                            "CI - failing", "CI - no status", etc.)

Asserts:
  1. Every job's `conclusion` field is exactly the string "success".
     Jobs with conclusion None / in_progress / failure / cancelled /
     skipped are all REJECTed (the mirror must represent a SETTLED
     green run, not a partial one).
  2. badge.svg's <title> matches a tolerant "CI - passing" regex.
     Even if every job is success, a stale GH badge CDN (still showing
     "failing" or "no status") is treated as drift - we mirror the
     same green signal that the README badge foregrounds.

Purpose:
  Locks down the green signal at PR time. A developer who pulls,
  rebuilds, and runs `scripts/capture_ci.sh` can spot upstream-CI
  regressions deterministically without re-running the full GHA matrix.
  The mirror + this snapshot check surface drift between commits and
  upstream CI in seconds rather than minutes.

Wired via CMake `add_test` with EXCLUDE_FROM_ALL + LABELS "ci" so
default `ctest --output-on-failure` does NOT invoke it (CI builds
can't update ci/ - gh auth isn't viable inside the matrix), but
`ctest -L ci` (or `ctest -R test_ci_snapshot`) does.

Usage:
  python3 tests/test_ci_snapshot.py
  python3 tests/test_ci_snapshot.py --repo-root /path/to/aster

Exit codes:
  0  mirror is all-green + badge matches expected passing token
  1  one or more job conclusions != success, OR badge mismatch
  2  parse error (missing file / malformed JSON / unreadable SVG / jobs
     empty / badge has no <title> element)
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent

# Files we expect at <repo_root>/ci/.
JOBS_FILE = "last_run_jobs.json"
BADGE_FILE = "badge.svg"

# Badge title regex. Match `CI - passing` case-insensitive, with
# whitespace flex between the dashes (handles any upstream rendering
# variation). Reject "failing", "no status", "pending", etc.
BADGE_PASSING_RE = re.compile(r"CI\s*-\s*passing", re.IGNORECASE)

# Title extraction regex (first <title>...</title> in the SVG).
RE_TITLE = re.compile(r"<title>([^<]+)</title>")


def _extract_badge_title(badge_text: str) -> str | None:
    """Return the contents of the first <title>...</title> element in
    `badge_text`, or None if no title is present.

    Used both for the assertion (must equal "passing") and for the
    feedback message when the assertion fails (so the developer can
    see what GH currently says).
    """
    m = RE_TITLE.search(badge_text)
    return m.group(1).strip() if m else None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help="Path to repo root (default: parent of this script).",
    )
    args = parser.parse_args(argv)

    repo_root: Path = args.repo_root.resolve()
    ci_dir = repo_root / "ci"

    jobs_path = ci_dir / JOBS_FILE
    badge_path = ci_dir / BADGE_FILE

    # Pre-flight: both files must exist. Otherwise the mirror hasn't
    # been captured yet (or has been pruned).
    missing: list[str] = []
    if not jobs_path.exists():
        missing.append(str(jobs_path))
    if not badge_path.exists():
        missing.append(str(badge_path))
    if missing:
        sys.stderr.write(
            "ERROR: ci/ mirror files missing. Run `scripts/capture_ci.sh` "
            "first to refresh them, then re-run this test.\n"
        )
        for f in missing:
            sys.stderr.write(f"  {f}\n")
        return 2

    # Jobs JSON parse + type check.
    try:
        jobs_obj = json.loads(jobs_path.read_text())
    except (json.JSONDecodeError, OSError) as e:
        sys.stderr.write(
            f"ERROR: cannot parse {jobs_path}: {e}\n"
        )
        return 2
    if not isinstance(jobs_obj, list):
        sys.stderr.write(
            f"ERROR: {jobs_path} should hold a top-level JSON array of "
            f"jobs, got {type(jobs_obj).__name__}\n"
        )
        return 2
    if not jobs_obj:
        # Empty list = no evidence of green signal. Reject: a developer
        # who runs capture_ci.sh against an empty repo or a brand-new
        # branch with no runs yet would see this; the fix is either
        # to retry once GH settles the runs.json fetch, or to act on
        # the surface-area "0 runs captured".
        sys.stderr.write(
            f"ERROR: {jobs_path} is empty (0 jobs) - no evidence of CI "
            f"signal. Either the mirror is stale or upstream has no "
            f"recorded runs yet; re-run scripts/capture_ci.sh.\n"
        )
        return 2

    # Audit each job's conclusion.
    failures: list[tuple[str, object]] = []
    for j in jobs_obj:
        name = j.get("name", "<unnamed>")
        conclusion = j.get("conclusion")
        if conclusion != "success":
            failures.append((name, conclusion))

    # Badge parse + passing-token match.
    try:
        badge_text = badge_path.read_text()
    except OSError as e:
        sys.stderr.write(f"ERROR: cannot read {badge_path}: {e}\n")
        return 2

    badge_title = _extract_badge_title(badge_text)
    if badge_title is None:
        sys.stderr.write(
            f"ERROR: badge.svg has no <title> element - cannot verify "
            f"the green signal token. Was this generated by GH Actions "
            f"or some other happy-eyeballs cache?\n"
        )
        return 2
    badge_pass = bool(BADGE_PASSING_RE.search(badge_title))

    # Report the worst of the two failure modes first so the developer
    # has the most-actionable signal. Job-conclusion failures come
    # first because they're the canonical "matrix regressed" signal;
    # badge mismatch is a downstream CDN staleness symptom.
    if failures:
        sys.stderr.write(
            f"=== {len(failures)}/{len(jobs_obj)} jobs have non-success "
            f"conclusions ===\n"
        )
        for name, conclusion in failures:
            sys.stderr.write(
                f"  {name:30s} -> conclusion={conclusion!r}\n"
            )
        sys.stderr.write(
            f"\nFAILURE: not every job is success; upstream CI matrix "
            f"is RED. Inspect the offending jobs' logs in the upstream "
            f"run (see ci/runs.json for the databaseId).\n"
        )
        return 1

    if not badge_pass:
        sys.stderr.write(
            f"=== badge.svg title does NOT match the 'CI - passing' "
            f"token ===\n"
            f"  extracted title: {badge_title!r}\n"
            f"  expected regex:  CI - passing (case-insensitive)\n"
            f"\n"
            f"Even though every job's conclusion is success, the badge "
            f"CDN is stale (this happens when all 5 jobs finish but the "
            f"badge SVG hasn't re-rendered yet). Re-run capture_ci.sh "
            f"in a few minutes to confirm.\n"
        )
        return 1

    sys.stdout.write(
        f"[ok] ci_snapshot: all {len(jobs_obj)} jobs have conclusion="
        f"success; badge title matches 'CI - passing'.\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
