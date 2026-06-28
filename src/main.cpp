// Aster — CLI entry point for replay-based backtest.
//
// Usage: aster_replay --itch-file <path> [options]

#include "aster/strategy/backtest.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace aster::strategy;

int main(int argc, char** argv) {
  BacktestConfig config;
  std::string out_pnl;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--itch-file") == 0 && i + 1 < argc) {
      config.itch_file = argv[++i];
    } else if (std::strcmp(argv[i], "--pool-size") == 0 && i + 1 < argc) {
      config.pool_size = static_cast<std::uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--num-symbols") == 0 && i + 1 < argc) {
      config.num_symbols = static_cast<std::uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
      std::string s = argv[++i];
      if (s == "realtime")
        config.replay.mode = ReplayConfig::SpeedMode::RealTime;
      else if (s == "batch")
        config.replay.mode = ReplayConfig::SpeedMode::Batch;
      else
        config.replay.speed_factor = std::stod(s);
    } else if (std::strcmp(argv[i], "--latency-exch") == 0 && i + 1 < argc) {
      config.replay.latency_exch_to_trader_ns =
          static_cast<std::uint64_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--latency-trader") == 0 && i + 1 < argc) {
      config.replay.latency_trader_to_exch_ns =
          static_cast<std::uint64_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--out-pnl") == 0 && i + 1 < argc) {
      out_pnl = argv[++i];
    } else if (std::strcmp(argv[i], "--help") == 0) {
      std::printf("Usage: aster_replay --itch-file <path> [--pool-size <n>] "
                  "[--num-symbols <n>] [--speed <realtime|batch|N>] "
                  "[--latency-exch <ns>] [--latency-trader <ns>] "
                  "[--out-pnl <path>]\n");
      return 0;
    }
  }

  if (config.itch_file.empty()) {
    std::fprintf(stderr, "Error: --itch-file is required. Use --help for usage.\n");
    return 1;
  }

  Backtest bt(std::move(config));
  bt.run();

  if (!out_pnl.empty()) {
    bt.analytics().write_pnl_csv(out_pnl);
  }

  return 0;
}
