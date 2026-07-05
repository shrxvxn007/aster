#!/usr/bin/env python3
"""
test_cli_flag_parser.py - CLI flag-parser regression gate.

Asserts that every `--flag` passed to `aster_replay` or `aster_sim` in any
.sh / .py / .yml callsite under the repo is registered in the relevant
binary's CLI parser:
  * aster_replay flags  -> src/main.cpp's std::strcmp parser
  * aster_sim    flags  -> src/sim.cpp's std::strcmp parser

Catches the class of bug where, e.g., someone gravity-scrolls past a flag
rename and leaves a stale `--input` in a shell script after the binary is
moved to `--itch-file`. Without this gate, the bug only surfaces the next
time that script runs; with it, `ctest --output-on-failure` (in the
.github/workflows/ci.yml build job's matrix) fails at PR time with a
file:line reference.

Usage:
    python3 tests/test_cli_flag_parser.py
    python3 tests/test_cli_flag_parser.py --repo-root /path/to/aster

Exit codes:
  0  every flag passed by callsites is recognized by its binary's parser
  1  one or more flag misses (output lists each with file:line)
  2  parse error (parser file missing / callsite unreadable)
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent

# File extensions to scan. .md/.json/etc are excluded by design: only
# invocations and CI configs drive this gate, not documentation.
SCAN_EXTS = {".sh", ".py", ".yml", ".yaml"}

# Directories pruned from the walk. build-* dirs are CMake output; .git
# would be wasteful; ci/ holds the offline mirror; Testing/ is CMake's.
SKIP_DIRS = {"build", "build-rel", "build-rel-lo", "build-ubsan",
             ".git", "ci", "Testing", "node_modules"}

# Binary-to-parser-file mapping. Keeping this explicit (not discovered)
# means new binaries must be added on purpose, preventing accidental
# mirror-the-wrong-parser scenarios.
BINARY_TO_PARSER: dict[str, str] = {
    "aster_replay": "src/main.cpp",
    "aster_sim":    "src/sim.cpp",
}

# Regex for a binary mention on a line. Word-boundary so `my_aster_sim`
# doesn't false-positive.
RE_BINARY = re.compile(r"\b(aster_replay|aster_sim)\b")

# Flag-token regex. Captures the canonical `--flag-name` form so we
# can compare directly against the parser-side flag set (which now also
# captures with the `--` prefix).
RE_FLAG = re.compile(r"--[a-zA-Z][a-zA-Z0-9-]*")

# Whole-line comment skipper. Pure comments (leading whitespace + `#`)
# are out: any flag tokens inside are not invocations. Inline trailing
# comments are handled implicitly by the walk-back below, which
# REJECTs on `#` via the DOC_MARKERS set.
RE_COMMENT_LINE = re.compile(r"^\s*#")

# Parser-side regex. Captures the FULL `--flag-name` literal INCLUDING
# the leading `--` so the audit compares like-for-like with callsite
# `--flag-name` tokens. Both `==` and bare-comparison forms match.
RE_PARSER_FLAG = re.compile(
    r'strcmp\s*\(\s*argv\s*\[\s*[a-zA-Z0-9_]+\s*\]'
    r'\s*,\s*"(--[a-zA-Z][a-zA-Z0-9-]*)"'
)

# DOC-only character set. ACCEPT anything else in the prefix. Real shell
# prefixes tolerate a wide char class (whitespace, alphanumerics,
# `. / - _`, plus every shell metachar `$ & | ; ( ) < > = { } [ ]`).
# We REJECT only when one of these specific chars appears in the
# prefix; each has a definite prose / string-literal / doc-reference
# meaning that wouldn't appear in a real shell command prefix:
#   "    double quote (python/bash string literal)
#   '    single quote (python/bash string literal)
#   `    markdown-style backtick reference
#   *    markdown-list bullet, glob, or doc emphasis
#   ,    list separator / list argument
#   <    process-substitution opening / HTML tag / YAML block indicator
#   >    process-substitution closing / HTML tag / YAML block indicator
# We do NOT include `#` here because the upstream RE_COMMENT_LINE
# filter already rejects whole-line shell comments whose first non-WS
# is `#`. Inline trailing comments are rare enough that we'd rather
# tolerate them than add complexity.
# DOC-only character set. ACCEPT anything else in the prefix. Real shell
# prefixes tolerate a wide char class (whitespace, alphanumerics,
# `. / - _`, plus every shell metachar `$ & | ; ( ) < > = { } [ ]`).
# We REJECT only when one of these specific chars appears in the
# prefix; each has a definite prose / string-literal / doc-reference
# meaning that wouldn't appear in a real shell command prefix:
#   "    double quote (python/bash string literal)
#   '    single quote (python/bash string literal)
#   `    markdown-style backtick reference
#   *    markdown-list bullet, glob, or doc emphasis
#   ,    list separator / list argument
#   <    process-substitution opening / HTML tag / YAML block indicator
#   >    process-substitution closing / HTML tag / YAML block indicator
#   #    inline shell comment marker; closes the trailing-inline-comment
#        gap left by RE_COMMENT_LINE (which only catches whole-line
#        comments whose first non-WS char is `#`)
DOC_MARKERS = frozenset('"\'`*,<>#')


def get_parser_flags(repo_root: Path, parser_relpath: str) -> set[str]:
    """Extract the set of flag names registered in the parser file.

    Reads source text once and pulls out every "--name" that sits
    inside a std::strcmp(argv[i], "--...") check. Both `==` and bare
    forms are matched by the same regex.
    """
    parser_path = repo_root / parser_relpath
    if not parser_path.exists():
        sys.stderr.write(f"ERROR: parser file not found: {parser_path}\n")
        sys.exit(2)
    return set(RE_PARSER_FLAG.findall(parser_path.read_text()))


def is_invocation(line: str, m_start: int) -> bool:
    """Determine if the binary mention at `m_start` is a real shell
    invocation versus an argument-slot mention, prose reference, or
    string literal.

    Three accept branches:
      (A) Bareword: m_start == 0 or all-whitespace-before (line starts
          with the binary, possibly indented via tab/space).
      (B) Path-component: char immediately before is `/`. Real shell
          invocations like `./build/X`, `/opt/X`, `$(/X`,
          `cd /x; ./build/X`, `( cd "$ROOT" && ./X )` match.

    The path-component branch additionally REJECTs iff any DOC_MARKERS
    char appears in the prefix - that catches prose/string-literal
    references like `help="Path to X"`, `Wraps \`X\``, and
    `<(./X)` (used only in docstring examples in this repo).

    Default: REJECT (used for argument-slot lines like
    `cmake --target aster_sim --parallel` where char-before-binary is
    space, falling through both branches).
    """
    head = line[:m_start]
    if head.strip() == "":
        # Pure whitespace before binary - line begins with the binary
        # (perhaps indented). Real bareword invocation. Subsumes the
        # m_start == 0 case: empty string's strip() is empty.
        return True
    if line[m_start - 1] == "/":
        # Path-component suffix: reject ONLY if a prose/string-literal
        # marker appears anywhere in the prefix. Allows shell-real
        # prefixes with meta chars ($, &, ;, etc.) through.
        return not any(c in DOC_MARKERS for c in head)
    return False


def find_callsite_flags(
    repo_root: Path,
) -> dict[str, list[tuple[Path, int, str, str]]]:
    """Walk every .sh/.py/.yml under repo_root; record (binary, flag) hits.

    Per non-comment line that mentions a binary, scan text AFTER the
    binary mention for `--flag` tokens. Pure-comment lines are skipped
    via RE_COMMENT_LINE so docstrings/notes don't pollute. Multi-line
    shell continuations are NOT stitched together; each line is
    scanned independently.

    Each binary mention is run through `is_invocation()` to reject
    argument-slot mentions like `cmake --target aster_sim --parallel`
    and prose references like `Wraps \`aster_sim --events\`` or
    `help="...build/aster_sim"`.

    Returns {binary_name: [(path, line_no, line_text, flag), ...]}
    """
    out: dict[str, list[tuple[Path, int, str, str]]] = {
        binary: [] for binary in BINARY_TO_PARSER
    }
    for root, dirs, files in os.walk(repo_root):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for fn in files:
            ext = os.path.splitext(fn)[1]
            if ext not in SCAN_EXTS:
                continue
            path = Path(root) / fn
            try:
                lines = path.read_text().splitlines()
            except (UnicodeDecodeError, OSError) as e:
                sys.stderr.write(f"ERROR: cannot read {path}: {e}\n")
                sys.exit(2)
            for i, line in enumerate(lines, start=1):
                if RE_COMMENT_LINE.match(line):
                    continue
                for m in RE_BINARY.finditer(line):
                    if not is_invocation(line, m.start()):
                        continue
                    bin_name = m.group(1)
                    after = line[m.end():]
                    for fm in RE_FLAG.finditer(after):
                        out[bin_name].append(
                            (path, i, line.rstrip(), fm.group(0))
                        )
    return out


def audit_binary(
    binary: str,
    hits: list[tuple[Path, int, str, str]],
    parser_flags: set[str],
) -> list[tuple[Path, int, str, str]]:
    """Return one row per distinct unrecognized flag, taking the first
    callsite occurrence as the anchor.

    De-dup so the same flag repeated across many sites isn't reported
    30 times - the first occurrence carries the file:line, which is
    the only one a fixer needs.
    """
    seen_misses: set[str] = set()
    misses: list[tuple[Path, int, str, str]] = []
    for row in hits:
        _, _, _, flag = row
        if flag in parser_flags:
            continue
        if flag in seen_misses:
            continue
        seen_misses.add(flag)
        misses.append(row)
    return misses


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

    parser_flags: dict[str, set[str]] = {
        binary: get_parser_flags(repo_root, rel)
        for binary, rel in BINARY_TO_PARSER.items()
    }

    callsites = find_callsite_flags(repo_root)

    total_hits = 0
    total_misses = 0
    failed_binaries: list[str] = []

    for binary in BINARY_TO_PARSER:
        hits = callsites[binary]
        total_hits += len(hits)
        misses = audit_binary(binary, hits, parser_flags[binary])
        if misses:
            total_misses += len(misses)
            failed_binaries.append(binary)
            sys.stderr.write(
                f"=== {binary}: flags passed in callsites but UNRECOGNIZED ===\n"
            )
            for path, line, text, flag in misses:
                rel = path.relative_to(repo_root)
                sys.stderr.write(
                    f"  {rel}:{line}  {flag}\n"
                    f"    >>> {text.strip()}\n"
                )

    if total_misses:
        sys.stderr.write(
            f"\nFAILURE: {total_misses} flag(s) drift between "
            f"{len(failed_binaries)}/{len(BINARY_TO_PARSER)} binary(ies) "
            f"and their parser(s). Fix by adding the flag(s) to "
            f"{', '.join(BINARY_TO_PARSER[b] for b in failed_binaries)} "
            f"or removing the offending call.\n"
        )
        return 1

    sys.stdout.write(
        f"[ok] cli_flag_parser: all {total_hits} "
        f"callsite flag invocations recognized by their binary's parser "
        f"(aster_replay -> {BINARY_TO_PARSER['aster_replay']}, "
        f"aster_sim -> {BINARY_TO_PARSER['aster_sim']})\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
