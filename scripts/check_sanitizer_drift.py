#!/usr/bin/env python3
"""
check_sanitizer_drift.py — fail when a build log shows unresolved sanitizer
runtime references (compile/link drift) so the workflow surfaces an
actionable message instead of a bare `gmake: *** Error 2`.

Usage:
  python3 scripts/check_sanitizer_drift.py /tmp/build.log
  cmake --build build 2>&1 | python3 scripts/check_sanitizer_drift.py

Exit codes:
  0  no sanitizer compile/link drift detected
  1  drift detected (one or more unresolved __asan_/__ubsan_/__tsan_ symbols)
  2  I/O error reading the input file

Why "as early as possible" is a tiny constraint here: the failing linkage
happens inside the cmake --build step itself. We can't pre-empt the linker
without knowing what flags each target will use, which would require a
non-trivial CMake introspection layer (generator expressions aren't
evaluable at configure time across target boundaries). Instead we run this
detector with `if: failure()` so it executes against the captured log when
the upstream Build step fails, and converts the opaque `Error 2` into a
precise "ASan runtime not linked for target X — CMS compile_options has
`-fsanitize=address` but link_options doesn't" diagnosis.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Each family maps to a handler shape. We emit one entry per (linker, family)
# combination so the summary groups hits by BOTH the family AND the linker
# that surfaced the error. Anchoring on `undefined` prevents false positives
# for documentation or source-file lines that happen to mention `__asan_`.
FAMILIES: tuple[tuple[str, str], ...] = tuple(
    entry
    for prefix, family in (
        ("__asan_",  "AddressSanitizer (ASan)"),
        ("__ubsan_", "UndefinedBehaviorSanitizer (UBSan)"),
        ("__tsan_",  "ThreadSanitizer (TSan)"),
    )
    for entry in (
        # GNU ld / bfd:   `undefined reference to `__asan_init'`
        #                  `undefined reference to '__ubsan_handle_*'
        (
            rf"undefined reference to\s+[`'\"]?{re.escape(prefix)}[A-Za-z0-9_]*",
            f"{family} runtime not linked — compile options emitted "
            f"{prefix}* calls (GNU ld / bfd)",
        ),
        # Clang LLD on Linux:   `undefined symbol: __asan_init`
        (
            rf"undefined symbol\s*:\s+[`'\"]?{re.escape(prefix)}[A-Za-z0-9_]*",
            f"{family} runtime not linked (Clang LLD)",
        ),
        # Apple ld (only relevant if a macOS-Debug matrix entry is ever
        # re-enabled — CI currently excludes it because UBSan runtime
        # overhead on Apple Clang is too high for the GHA runner, but
        # `Undefined symbols for architecture arm64:` shows up if the
        # symbol is prefixed with `_`):
        #     `Undefined symbols for architecture arm64:`
        #     `  "___asan_init", referenced from:`
        (
            rf"\"_*{re.escape(prefix)}[A-Za-z0-9_]*\".*referenced from",
            f"{family} runtime not linked (Apple ld)",
        ),
        # accel: `__ubsan_*` symbol without prefix underscore
        (
            rf"undefined reference to\s+[`'\"]?{re.escape(prefix.lstrip('_'))}[A-Za-z0-9_]*",
            f"{family} runtime not linked (no underscore)",
        ),
    )
)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Detect sanitizer compile/link drift in a CMake build log.",
    )
    parser.add_argument(
        "logfile",
        nargs="?",
        type=str,
        help="Path to build log. Read from stdin if omitted or '-'.",
    )
    args = parser.parse_args(argv[1:])

    if args.logfile is None or args.logfile == "-":
        text = sys.stdin.read()
    else:
        path = Path(args.logfile)
        try:
            text = path.read_text(errors="replace")
        except OSError as exc:
            print(f"ERROR: cannot read {path}: {exc}", file=sys.stderr)
            return 2

    lines = text.split("\n")
    hits_by_cause: dict[str, list[tuple[int, str]]] = {}
    for pat, cause in FAMILIES:
        for m in re.finditer(pat, text):
            line_no = text.count("\n", 0, m.start()) + 1
            hits_by_cause.setdefault(cause, []).append(
                (line_no, lines[line_no - 1].strip())
            )

    if not hits_by_cause:
        print("[OK] no sanitizer compile/link drift detected in build log")
        return 0

    total = sum(len(v) for v in hits_by_cause.values())
    print(
        f"[DRIFT] {total} unresolved sanitizer runtime symbol(s) in build log:",
        file=sys.stderr,
    )
    for cause, hits in sorted(hits_by_cause.items(), key=lambda kv: -len(kv[1])):
        print(f"  {cause}  ({len(hits)} unique hits)", file=sys.stderr)
        for line_no, line in hits[:5]:
            print(f"    L{line_no}: {line}", file=sys.stderr)
        if len(hits) > 5:
            print(f"    ... ({len(hits) - 5} more in this family)", file=sys.stderr)

    print("", file=sys.stderr)
    print("Why this happens:", file=sys.stderr)
    print(
        "  target_compile_options(<t>) adds -fsanitize=<family> but the\n"
        "  matching target_link_options(<t>) does not carry the same flag,\n"
        "  so the linker can't pull in the sanitizer runtime that the\n"
        "  compiled object files reference.",
        file=sys.stderr,
    )
    print("", file=sys.stderr)
    print("How to fix (try in order):", file=sys.stderr)
    print(
        "  1. Mirror the -fsanitize=… flag into target_link_options for\n"
        "     the same target. (See CMakeLists.txt's `aster_obj` block for\n"
        "     the canonical pattern.)\n"
        "  2. If the sanitizer is meant only for aster_obj, link with the\n"
        "     same flags on every consumer: aster_sim, aster_replay,\n"
        "     test_engine, test_parser, test_replay.\n"
        "  3. Clean the ccache + build dir to rule out a stale-ASan\n"
        "     object file linked against a UBSan-only rebuild:\n"
        "       ccache -C && rm -rf build && cmake -B build …",
        file=sys.stderr,
    )

    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
