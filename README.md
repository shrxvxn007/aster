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

CMake ≥ 3.16 is required. The build produces two executables and two test
binaries under `build/aster/`:

```
build/aster/aster_replay   # ITCH replay + backtest CLI
build/aster/aster_sim      # synthetic engine benchmark (Poisson arrivals)
build/aster/test_engine    # engine unit tests
build/aster/test_parser    # ITCH parser tests
```

`Release` configures `-O3 -flto -march=native` and disables
`<cassert>`. `Debug` enables AddressSanitizer + UBSan and frame
pointers for diagnosis.

---

## Run

### `aster_replay` — replay an ITCH file with a market-making backtest

```sh
./build/aster/aster_replay --itch-file path/to/file.itch
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
./build/aster/aster_sim --pool 100000 --events 1000000 --symbols 16
```

Generates a deterministic Poisson order-arrival process (`splitmix64` PRNG),
sends `N` events round-robin across `S` symbols through the engine, and
prints throughput (events / second) plus a nanosecond-latency profile.

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
`perf-floor` job). On a typical Apple M-series laptop or modern Intel
server the engine sustains 30–80 M events/s single-threaded — see
`--speed realtime` to replicate the live-feel timing probe.

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
