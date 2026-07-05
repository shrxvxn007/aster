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
          static_cast<std::uint64_t>(std::atol(argv[++i]));
    } else if (std::strcmp(argv[i], "--latency-trader") == 0 && i + 1 < argc) {
      config.replay.latency_trader_to_exch_ns =
          static_cast<std::uint64_t>(std::atol(argv[++i]));
    }
    // Strategy parameters.
    else if (std::strcmp(argv[i], "--gamma") == 0 && i + 1 < argc) {
      config.strategy.gamma = std::stod(argv[++i]);
    } else if (std::strcmp(argv[i], "--sigma") == 0 && i + 1 < argc) {
      config.strategy.sigma = std::stod(argv[++i]);
    } else if (std::strcmp(argv[i], "--kappa") == 0 && i + 1 < argc) {
      config.strategy.kappa = std::stod(argv[++i]);
    } else if (std::strcmp(argv[i], "--base-spread") == 0 && i + 1 < argc) {
      config.strategy.base_spread = std::stod(argv[++i]);
    } else if (std::strcmp(argv[i], "--inventory-limit") == 0 && i + 1 < argc) {
      config.strategy.inventory_limit = std::stod(argv[++i]);
    }
    // Risk limits.
    else if (std::strcmp(argv[i], "--position-limit") == 0 && i + 1 < argc) {
      config.risk.position_limit = static_cast<std::int64_t>(std::atol(argv[++i]));
    } else if (std::strcmp(argv[i], "--max-orders-per-window") == 0 && i + 1 < argc) {
      config.risk.max_orders_per_window = static_cast<std::uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--throttle-window-ns") == 0 && i + 1 < argc) {
      config.risk.throttle_window_ns = static_cast<std::uint64_t>(std::atol(argv[++i]));
    } else if (std::strcmp(argv[i], "--max-drawdown") == 0 && i + 1 < argc) {
      config.risk.max_drawdown = std::stod(argv[++i]);
    }
    // Analytics / fees.
    else if (std::strcmp(argv[i], "--maker-fee") == 0 && i + 1 < argc) {
      config.analytics.maker_fee_per_share = std::stod(argv[++i]);
    } else if (std::strcmp(argv[i], "--taker-fee") == 0 && i + 1 < argc) {
      config.analytics.taker_fee_per_share = std::stod(argv[++i]);
    }
    // Output.
    else if (std::strcmp(argv[i], "--out-pnl") == 0 && i + 1 < argc) {
      out_pnl = argv[++i];
    } else if (std::strcmp(argv[i], "--help") == 0) {
      std::printf(
          "Usage: aster_replay --itch-file <path> [options]\n"
          "\n"
          "Replay / latency:\n"
          "  --speed <realtime|batch|N>  Replay speed mode (default: batch)\n"
          "  --latency-exch <ns>         Exchange→trader one-way latency\n"
          "  --latency-trader <ns>       Trader→exchange one-way latency\n"
          "\n"
          "Strategy (Avellaneda-Stoikov):\n"
          "  --gamma <float>             Risk aversion (default: 0.1)\n"
          "  --sigma <float>             Volatility estimate (default: 0.02)\n"
          "  --kappa <float>             Book liquidity (default: 1.0)\n"
          "  --base-spread <float>       Minimum half-spread (default: 0.01)\n"
          "  --inventory-limit <float>   Position limit for skew (default: 100)\n"
          "\n"
          "Risk:\n"
          "  --position-limit <int>      Hard max position (default: 100)\n"
          "  --max-orders-per-window <int>  Order rate limit (default: 100)\n"
          "  --throttle-window-ns <int>  Rate-limit window (default: 1s)\n"
          "  --max-drawdown <float>      Drawdown kill-switch (0=off)\n"
          "\n"
          "Fees / output:\n"
          "  --maker-fee <float>         Maker fee/share (default: 0.0001)\n"
          "  --taker-fee <float>         Taker fee/share (default: 0.0002)\n"
          "  --out-pnl <path>            Write equity-curve CSV\n");
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
