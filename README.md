# Aster – C++20 Low‑Latency Matching Engine & Market‑Making Simulator

A high‑performance event‑driven limit order book with ITCH‑style replay,
nanosecond latency profiling, and a realistic market‑making backtester.

## Features
- Cache‑friendly order book (intrusive lists, pool allocator, flat price levels)
- Hardware timestamping via `rdtsc` with calibration
- Deterministic replay from incremental ITCH‑style binary feed
- Configurable latency injection, speed control, and MM reaction delay
- Market‑making strategy with inventory, flow, and volatility awareness
- L3‑accurate fill‑probability model (`exp(-shares_ahead / avg_trade_size)`)
- Fill‑probability validation (predicted vs actual)
- Multi‑horizon adverse selection (markout) analysis
- Sharpe ratio (with optional constant event spacing)
- Lock‑free SPSC queue with cache‑line isolation
- Unit tests, benchmark mode, and multi‑threaded throughput example

## Build
```bash
mkdir build && cd build
cmake .. && make -j
