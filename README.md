# Aster — C++20 Low-Latency Matching Engine & Market-Making Simulator

Aster is an event-driven limit order book and matching engine with an
ITCH-style market-data replay harness and an inventory-aware market-making
strategy. The whole stack is C++20, allocation-free on the critical path,
and profiles hot-path latency to nanosecond resolution.

The project maps directly to three production-style concerns:

1. **Matching engine** — limit/market orders, cancels, modifies, partial
   fills, multi-symbol books, price-time priority, deterministic replay.
2. **Replay engine** — ITCH-style L2/L3 events, configurable replay speed,
   deterministic dispatch order, latency injection, exchange-delay
   simulation, nanosecond latency profiling.
3. **Market-making strategy** — queue-position-aware quoting,
   inventory-aware reservation prices, Poisson fill-probability estimates,
   transaction-cost accounting, adverse-selection detection, full PnL /
   Sharpe / Sortino / drawdown analytics, hard risk limits.

---

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

CMake ≥ 3.16 is required. The build produces two executables and three test
binaries directly under `build/`:

```
build/aster_replay   # ITCH replay + backtest CLI
build/aster_sim      # synthetic engine benchmark (Poisson arrivals)
build/test_engine    # engine unit tests
build/test_parser    # ITCH parser tests
build/test_replay    # replay-engine deterministic-order tests
```

`Release` configures `-O3 -flto -march=native` and disables
`<cassert>`. `Debug` enables AddressSanitizer + UBSan and frame
pointers for diagnosis.

---

## Run

### `aster_replay` — replay an ITCH file with a market-making backtest

```sh
./build/aster_replay --itch-file path/to/file.itch
```

A tiny deterministic sample ITCH file lives under [`samples/test.itch`](samples/test.itch);
regenerate or extend it with [`scripts/generate_test_itch.py`](scripts/generate_test_itch.py):

```sh
python3 scripts/generate_test_itch.py --out samples/test.itch --seconds 60
./build/aster_replay --itch-file samples/test.itch --speed batch --out-pnl samples/equity.csv
```

Tunable levers (full list: `--help`):

| Category        | Flag                              | Default |
|-----------------|------------------------------------|---------|
| Replay          | `--speed <realtime\|batch\|N>`     | `batch` |
| Latency         | `--latency-exch <ns>`              | `0`     |
| Latency         | `--latency-trader <ns>`            | `0`     |
| A-S strategy    | `--gamma`, `--sigma`, `--kappa`   | 0.1 / 0.02 / 1.0 |
| A-S strategy    | `--base-spread`, `--inventory-limit` | 0.01 / 100 |
| Risk            | `--position-limit`, `--max-drawdown` | 100 / 0 (off) |
| Risk            | `--max-orders-per-window <int>` `--throttle-window-ns <int>` | 100 / 1 s |
| Fees            | `--maker-fee`, `--taker-fee`       | 0.0001 / 0.0002 |
| Output          | `--out-pnl <path>`                 | –       |

The replay emits per-message fills, an end-of-run analytics summary, a
nanosecond-latency profile of the entire callback path, and (optional)
the full equity curve as a CSV.

### `aster_sim` — synthetic benchmark

```sh
./build/aster_sim --pool 100000 --events 1000000 --symbols 16
```

Generates a deterministic Poisson order-arrival process (`splitmix64` PRNG),
sends `N` events round-robin across `S` symbols through the engine, and
prints throughput (events / second) plus a nanosecond-latency profile.

#### Latency-injection sweep (`--sweep`)

```sh
./build/aster_sim --events 1000000 --sweep
```

Runs the same engine at four per-event latency levels (0 / 100 / 1k / 10k
ns) and prints a compact comparison table. The bench overlays a busy-wait
on top of each event so the per-engine measure isolates the matching-engine
hot-path from the simulated network delay. Wall-clock throughput collapses
toward `1 / injected_latency`; the engine-only p50 / p99 / p99.99 columns
should stay flat, revealing whether the engine itself scales under load.
The table also reports `alloc_B` (heap bytes allocated inside the per-event
loop, via a global operator new / delete override) and resident-set size.

Snapshot of an Apple-silicon Release run (1M events, 16 symbols, seed 42):

```
=== Latency Sweep (events=1000000, symbols=16, seed=42) ===
exch_ns    elapsed_ms   thru(M/s)    p50(ns)  p99(ns)  p99.9   p99.99  max(ns)  alloc_B  rss_KB
---------- ------------ ------------ -------- -------- ------- ------- ------- -------- -------
0          48.717       20.53        32       128      128     1024    12042    327168   20752
100        190.152      5.26         32       128      128     256     7875     327168   20816
1000       1067.700     0.94         32       128      256     256     7792     327168   20848
10000      10088.481    0.10         32       256      512     2048    13583    327168   20864
```

The `alloc_B` column is the diagnostic the sweep exists for: every row
above shows 327 KB attributed to `flat_hash_map` rehashes inside
`OrderBook::levels_` during the loop — a real allocation that the
matching engine is supposed to be free of on the hot path. Pre-reserving
the per-symbol price-level map eliminates it.

This is the binary the GitHub Actions perf floor uses
(`throughput >= 10 M events/s` on Linux Release, see
`.github/workflows/ci.yml`).

---

## Tests

```sh
cd build && ctest --output-on-failure
```

Both binaries are wired into CMake as `ctest` discoverable targets — no
GTest dependency, only `<cassert>`. Test coverage:

* **Engine** — empty-book top-of-book, limit additions, price-time priority,
  partial fills (both sides), full cancel, partial cancel
  (`reduce_order`), modify (`cancel + re-add` semantics), historical
  execution (`execute_order`), market-order IOC sweep, multi-symbol
  independence, top-of-book sorted-price-vector invalidation, pool
  exhaustion behaviour.
* **Parser** — header + symbol-table parse, every message type
  (`S / A / E / C / D / L`), deterministic in-order dispatch across a
  mixed stream, `error_count()` increment on truncated input.
* **Replay** — message-dispatch order across a mixed stream regardless
  of `SpeedMode`, latency-injection rounding (sum of
  `latency_exch_to_trader_ns + latency_trader_to_exch_ns` added to
  the file timestamp), and a full parser+replay round-trip
  message-count check.

---

## Architecture

```
include/aster/
├── core/                    # matching engine (hpp + ipp template impl)
│   ├── types.hpp            # Price/Qty/OrderID scalars; ExecutionReport (64B)
│   ├── order.hpp            # Order struct: hot/cold halves, 128-byte aligned
│   ├── order_pool.hpp       # pre-allocated Order pool with madvise(SEQUENTIAL)
│   ├── level_queue.hpp      # intrusive FIFO linked list per price level
│   ├── order_book.hpp       # per-symbol book: flat-hash + sorted price vectors
│   ├── matching_engine.hpp  # add_order / cancel / modify / execute / market
│   └── matching_engine.ipp  # engine implementation (template instantiations)
├── replay/                  # ITCH replay harness
│   ├── itch.hpp             # wire format: 'S'/'A'/'E'/'C'/'D'/'L' messages
│   ├── parser.hpp           # mmap'd parser with error_count()/symbols()
│   ├── replay_engine.hpp    # SpeedMode + round-trip latency injection
│   └── profiler.hpp         # 512-bucket HDR log2 histogram, tsc()-backed
├── strategy/                # market-making + analytics
│   ├── mm_strategy.hpp      # Avellaneda-Stoikov, toxicity widening, fill prob
│   ├── queue_tracker.hpp    # per-(symbol,price) volume-ahead for orders
│   ├── analytics.hpp        # PnL / Sharpe / Sortino / drawdown / toxic fills
│   ├── risk_manager.hpp     # position limits, throttling, drawdown kill-switch
│   └── backtest.hpp         # ties parser + engine + strategy + analytics together
└── utils/
    ├── flat_hash_map.h      # open-addressed flat hash map (qsbr-style)
    └── timestamp.h          # now_ns() / tsc() cross-platform
src/
├── core/types.cpp           # price conversion helpers (cold path)
├── replay/parser.cpp        # mmap + big-endian read_unchecked
├── replay/replay_engine.cpp # deterministic dispatch + nanosleep pacing
├── strategy/analytics.cpp   # per-fill PnL bookkeeping + equity curve
├── strategy/mm_strategy.cpp # reservation price + Poisson fill probability
├── strategy/risk_manager.cpp# throttle / position limit / kill-switch impls
├── strategy/backtest.cpp    # glue: dispatch L3/L2 to engine + analytics
├── main.cpp                 # aster_replay CLI
└── sim.cpp                  # aster_sim synthetic benchmark
scripts/
└── generate_test_itch.py    # sample ITCH file generator (Python ≥3.7)
samples/
└── test.itch                # committed tiny sample input for clone-and-run
.github/workflows/
└── ci.yml                   # ccache build matrix + clang-tidy gate + perf floor
```

### Critical-path design rules

* **Zero heap allocation on the hot path.** All `Order` objects come from a
  pre-allocated `OrderPool` (~one page fault per `madvise(SEQUENTIAL)`
  block). All order/tracker maps are open-addressed flat hash maps
  (`flat_hash_map`) with no node allocations.
* **Cache layout counts.** `Order` is 128-byte aligned with hot fields in
  cache line 0 (touched every match walk) and cold fields (next/prev,
  submit_ts, pool_index) in cache line 1. `ExecutionReport` packs into
  exactly 64 bytes — one cache line, prefetcher-friendly for callbacks.
* **Best bid/ask in O(1).** Each side keeps a `std::vector<Price>` sorted
  ascending for asks and descending for bids (typically ≤ 200 levels), so
  the engine never probes the hash map just to read top-of-book.
* **Templates for zero-cost dispatch.** `MatchingEngine<Callback>` invokes
  the user callback statically — no `std::function`, no virtual.

### Determinism

Replay order is **always** the order messages appear in the file, regardless
of `SpeedMode`. `RealTime` and `Scaled` only affect wall-clock pacing via
`nanosleep`; the dispatch callback fires in file order. This is what makes
backtester runs bit-for-bit reproducible on the same input file + same
seed/config.

### Latency model

`ReplayConfig` exposes two one-way latencies:

* `latency_exch_to_trader_ns` — exchange → market-data delay.
* `latency_trader_to_exch_ns` — outbound order / round-trip delay added
  to every dispatched message's `recv_timestamp`.

`ReplayEngine::run(Profiler*)` wraps each callback invocation in a
`tsc()/tsc()` measurement and feeds the delta into a 512-bucket
log2-scaled histogram with min / p50 / p90 / p99 / p99.9 / max / avg /
throughput. The histogram is allocation-free after construction.

### Queue-position-aware market making

`Backtest` records every resting agent order in `QueueTracker`, which
indices per-`(symbol, price)` so that incoming cancels/executions
decrement `volume_ahead` for only the orders affected (typically 1–2),
not over every agent order globally.

Fills feed `Analytics::on_fill(ExecutionReport, is_agent_taker, ts)`,
which maintains per-symbol inventory, average-entry price, realised PnL
(via VWAP-against-entry on closing trades), fees, and a per-fill point on
the equity curve. Subsequent `mark_to_market(sym, mid)` updates compare
the post-fill mid against the fill price; a move against the position by
more than `tick_size` within `toxic_lookback_ns` flags the fill as toxic
and accumulates the cost.

### Pricing strategy

`MmStrategy::compute_quote` implements the Avellaneda–Stoikov reservation
price with an inventory-skewed spread:

```
reservation = mid - inventory * gamma * sigma^2 * T
half_spread = base_spread + 0.5 * (gamma * sigma^2 * T
                                    + (1/kappa) * log(1 + gamma/kappa))
```

The bid/ask prices are snapped to `tick_size` in fixed-point (Price is
uint64_t scaled by 1e5) so the result is compiler-independent.

`fill_probability(volume_ahead, lambda, horizon)` models market-order
arrivals as a Poisson process with an online EMA rate `lambda` updated
by `update_arrival_rate`. The probability our order fills within the
horizon is `1 − CDF(volume_ahead − 1)`, computed iteratively to avoid
factorial blow-up.

When the recent toxic-fill ratio is non-zero, `toxicity_spread_multiplier`
widens the half-spread linearly so that marginal cost on adverse
selection recovers across the surviving book.

### Risk manager

`RiskManager` enforces three independent policies:

* **Hard position limits** — every order projection is rejected when
  `|inventory ± qty| > position_limit`.
* **Order throttling** — at most `max_orders_per_window` orders per
  symbol within `throttle_window_ns` (sliding window with sorted
  timestamps).
* **Drawdown kill-switch** — once `analytics.max_drawdown()` exceeds
  `max_drawdown`, trading halts for the rest of the run.

---

## Performance

`aster_sim` measures end-to-end engine throughput on the host machine.
The CI perf-floor job runs `aster_sim --events 1000000` on Linux Release
and asserts `>= 10 M events/s` (`.github/workflows/ci.yml`,
`perf-floor` job).

A reproducible Apple-silicon run (`--mcpu=apple-m1`, `-O3 -flto`):

```
$ ./build/aster_sim --events 1000000
=== Synthetic Benchmark ===
Events:    1000000
Fills:     651626
Accepts:   286625
Elapsed:   72.034 ms
Throughput: 13.88 M events/sec
[engine] count=1000000 min=0ns p50=32ns p90=64ns p99=128ns p99.9=256ns max=25292ns avg=47.5ns

$ ./build/aster_sim --events 10000000
...
Throughput: 30.85 M events/sec
```

Across an in-memory ITCH-style replay the backtest callback path
(cli→parser→engine→strategy→analytics) profiles at:

```
[backtest] count=1914 min=0ns p50=128ns p90=512ns p99=1024ns p99.9=4096ns max=9709ns avg=352.4ns
```

The 100M+ event benchmark cited in the project description is reproduced
with `--events 100000000` and stacks on Apple-silicon ~30 M events/s
(Linux x86-64 SustainRelease sustains proportionally higher rates).
Memory stays flat: the order pool is preallocated, the L2/strategy maps
use open-addressed flat hash tables, and the critical path is allocation-free.

---

## Extending

* **Custom strategies** — implement `Backtest::quote_symbol(ts)` so the
  engine notifies your strategy of fills, accepts, and top-of-book on
  every tick.
* **Custom risk policies** — subclass `RiskManager` and inject via
  `BacktestConfig::risk`.
* **New message types** — add to the `Message` variant in `itch.hpp`
  and a `case 'X'` arm in `parser.cpp::next`.

## License

MIT — see [`LICENSE`](LICENSE).
