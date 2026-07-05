# ci/ — offline-verifiable CI artifacts

This directory captures the inputs and outputs of the GitHub Actions CI
workflow (`.github/workflows/ci.yml`) so a reader with no network access
can verify the project's build-status claim from this repository alone.

## Files

| File | What it is | When it's useful |
| ---- | ---------- | ---------------- |
| `badge.svg` | Verbatim download of `https://github.com/shrxvxn007/aster/actions/workflows/ci.yml/badge.svg`. Self-contained, renderable in any Markdown client. | Quick visual check that the build is currently passing. **Snapshot only**: may go stale between captures. |
| `runs.json` | Last N (default 3) GitHub Actions runs on `main`, as a pretty-printed JSON array. Each record carries `databaseId`, `headSha`, `startedAt`, `updatedAt`, `conclusion`, `url`, etc. Sorted by `databaseId` descending so `git diff` shows what actually changed. | Verify that the badge snapshot agrees with the recent run history, or fetch the actual URL for a specific run. |
| `last_run.json` | One-record JSON of the most-recent run: name, conclusion, head SHA, branch, status, timestamps, URL. | Source-of-truth for the current badge's "what triggered it" story. |
| `last_run_jobs.json` | All jobs in the latest run with per-job conclusions `(name, status, conclusion, startedAt, completedAt)`. | Drill into which matrix entry failed when the badge is red. |
| `last_commit.txt` | Output of `git log --oneline -n 1` at capture time. | Match the badge / runs to a specific Git commit. |
| `perf_floor_local.txt` | Local output of `./build/aster_sim --events 500000` on the maintainer's macOS arm64 box (this dev environment). **NOT the CI perf-floor step's stdout**. | Sanity-check that the throughput-floor shape (`Throughput: NN.NN M events/sec`) is realistic for this engine. |

## Caveat: the throughput floor

`perf_floor_local.txt` is a **maintainer-local mirror**, NOT the
authoritative CI perf-floor output. The actual awk-parsed Throughput
that the `perf-floor` job asserts `>= 10 M events/s` lives in the GitHub
Actions run log at:

```
https://github.com/shrxvxn007/aster/actions/runs/<databaseId>
```

(`<databaseId>` is in `runs.json`.) To produce a true offline mirror
of THAT value, re-run `scripts/capture_ci.sh` — it pulls the live
perf-floor job's stdout via `gh run view <id> --log` and writes
`ci/perf_floor_ci.txt`.

## Regenerating

Run the capture script from the repo root:

```
./scripts/capture_ci.sh                # uses default settings
./scripts/capture_ci.sh --runs 5       # capture 5 most-recent runs
```

The script is idempotent — re-running it overwrites the captured files
in place. It exits non-zero with a clear error if `gh` is not
authenticated, the upstream repo can't be reached, or the badge
download returns a non-2xx HTTP status.

## When to refresh

On-demand, before tagging a release or before merging a high-impact
change. Per-push capture would flicker the badge and pollute git
history with timestamp bumps; that's not the intent of this directory.
