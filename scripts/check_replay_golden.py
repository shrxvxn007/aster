#!/usr/bin/env python3
"""
check_replay_golden.py \u2014 strategy-layer regression gate.

Diff an `aster_replay` stdout capture against a golden JSON snapshot.

Usage:
  python3 scripts/check_replay_golden.py <golden.json> <(./aster_replay ...)

Exit code:
  0  all fields within tolerance
  1  one or more fields exceed their configured tolerance
  2  parse error (no Analytics block, malformed golden file, missing field)

The Analytics block is parsed line-by-line. Fields we care about are emitted
by Analytics::print() in `src/strategy/analytics.cpp` at exactly:

    === Analytics ===
      Realized PnL:   <float>
      Unrealized PnL: <float>
      Total Fees:     <float>
      Net PnL:        <float>
      Max Drawdown:   <float>
      Sharpe:         <float>
      Sortino:        <float>
      Turnover:       <float>
      Toxic fills:    <int> / <int> (<float>%)
      Avg toxic cost: <float>

The `Toxic fills` line carries three fields (toxic_fills_count, total_fills,
toxic_pct). Stdlib only; no dependencies.
"""
from __future__ import annotations

import json
import math
import re
import sys
from pathlib import Path
from typing import TextIO


# Parsing regexes. Anchor on the leading two-space indent so we don't pick
# up unrelated lines (e.g. the non-deterministic `[backtest]` profiler line,
# which the gate explicitly ignores).
RE_REALIZED   = re.compile(r"^\s+Realized PnL:\s+(-?\d+\.\d+)\s*$")
RE_UNREALIZED = re.compile(r"^\s+Unrealized PnL:\s+(-?\d+\.\d+)\s*$")
RE_FEES       = re.compile(r"^\s+Total Fees:\s+(-?\d+\.\d+)\s*$")
RE_NET        = re.compile(r"^\s+Net PnL:\s+(-?\d+\.\d+)\s*$")
RE_DRAWDOWN   = re.compile(r"^\s+Max Drawdown:\s+(-?\d+\.\d+)\s*$")
RE_SHARPE     = re.compile(r"^\s+Sharpe:\s+(-?\d+\.\d+)\s*$")
RE_SORTINO    = re.compile(r"^\s+Sortino:\s+(-?\d+\.\d+)\s*$")
RE_TURNOVER   = re.compile(r"^\s+Turnover:\s+(-?\d+\.\d+)\s*$")
RE_TOXIC      = re.compile(r"^\s+Toxic fills:\s+(\d+)\s+/\s+(\d+)\s+\((\d+\.\d+)%\)\s*$")
RE_AVG_TOXIC  = re.compile(r"^\s+Avg toxic cost:\s+(-?\d+\.\d+)\s*$")


# Maps the golden-JSON field name -> (parser regex, parser-callable for ints).
# 'kind' picks which tolerance bucket to use: 'currency' / 'ratio' /
# 'count_int' / 'count_pct'.
FIELD_PARSE = {
    "realized_pnl":   (RE_REALIZED,   "float"),
    "unrealized_pnl": (RE_UNREALIZED, "float"),
    "total_fees":     (RE_FEES,       "float"),
    "net_pnl":        (RE_NET,        "float"),
    "max_drawdown":   (RE_DRAWDOWN,   "float"),
    "sharpe":         (RE_SHARPE,     "float"),
    "sortino":        (RE_SORTINO,    "float"),
    "turnover":       (RE_TURNOVER,   "float"),
    "avg_toxic_cost": (RE_AVG_TOXIC,  "float"),
}


def parse_analytics(text: str) -> dict[str, float | int]:
    """Pull the Analytics block out of `text` and return a dict of every
    field the regression gate cares about."""
    out: dict[str, float | int] = {}
    for line in text.splitlines():
        for k, (rx, kind) in FIELD_PARSE.items():
            m = rx.match(line)
            if m:
                out[k] = float(m.group(1))
                break
        m = RE_TOXIC.match(line)
        if m:
            out["toxic_fills"] = int(m.group(1))
            out["total_fills"] = int(m.group(2))
            out["toxic_pct"]   = float(m.group(3))
    return out


def within(actual: float, expected: float, tol: float) -> bool:
    if math.isnan(actual) or math.isnan(expected):
        return actual == expected  # both NaN is the only way to be equal
    if tol <= 0.0:
        return actual == expected
    return abs(actual - expected) <= tol


def classify_tolerance(golden_meta: dict, key: str) -> float:
    """Return the absolute tolerance for a given field name."""
    t = golden_meta.get("tolerances", {})
    if key in ("toxic_fills", "total_fills"):
        return float(t.get("counts_exact", 0.0))
    if key == "toxic_pct":
        # toxic_pct is the rounded-integer / integer-percent field
        # from Analytics::print()'s `%.1f`. Exact match is correct.
        return 0.0
    if key in ("sharpe", "sortino", "avg_toxic_cost"):
        return float(t.get("ratio_abs", 1e-6))
    return float(t.get("currency_abs", 1e-4))


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        sys.stderr.write(
            f"Usage: {argv[0]} <golden.json> <replay-output.txt or ->\n")
        return 2

    golden_path = Path(argv[1])
    if not golden_path.exists():
        sys.stderr.write(f"Golden file not found: {golden_path}\n")
        return 2
    with golden_path.open() as f:
        golden = json.load(f)

    replay_arg = argv[2]
    if replay_arg == "-":
        replay_text = sys.stdin.read()
    else:
        with open(replay_arg) as f:
            replay_text = f.read()

    parsed = parse_analytics(replay_text)
    meta = golden.get("_meta", {})

    failures: list[str] = []
    missing: list[str] = []
    for k in golden:
        if k.startswith("_"):
            continue  # metadata
        if k not in parsed:
            missing.append(k)
            continue
        tol = classify_tolerance(meta, k)
        ok = within(parsed[k], golden[k], tol)
        if not ok:
            failures.append(
                f"  {k:18s} expected={golden[k]:.6f}  "
                f"actual={parsed[k]:.6f}  tol={tol:g}  "
                f"diff={parsed[k] - golden[k]:+.6f}"
            )

    if missing:
        sys.stderr.write(
            "=== GOLDEN PARSE FAILURES ===\n"
            "Missing fields in replay output:\n"
        )
        for k in missing:
            sys.stderr.write(f"  {k}\n")
        sys.stderr.write(
            f"\n(replay output did not contain all expected Analytics fields; "
            f"this usually means the build is stale or replay crashed)\n"
        )
        return 2

    if failures:
        sys.stderr.write("=== GOLDEN DIFF FAILURES ===\n")
        for line in failures:
            sys.stderr.write(line + "\n")
        sys.stderr.write(
            "\nThe strategy-layer regression gate FAILED.\n"
            "This usually means:\n"
            "  - the strategy algorithm changed (verify the diff is intended),\n"
            "  - the analytics/classifier has a bug (look at toxic_pct drift),\n"
            "  - the Golden needs an update (edit tests/golden/analytics_ci.json).\n"
        )
        return 1

    sys.stdout.write(
        f"[ok] check_replay_golden: {len(parsed)} fields within tolerance\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
