// Aster — Analytics.
//
// Tracks per-symbol inventory, realized/unrealized PnL, fees, equity curve,
// max drawdown, Sharpe, Sortino, turnover. Updated incrementally per fill.

#pragma once

#include "aster/core/types.hpp"

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

namespace aster::strategy {

struct SymbolState {
  std::int64_t inventory = 0;       // signed shares
  double avg_entry_price = 0.0;      // VWAP of current position
  double realized_pnl = 0.0;
  double fees = 0.0;
};

struct AnalyticsConfig {
  double maker_fee_per_share = 0.0001;  // exchange credit/debit
  double taker_fee_per_share = 0.0002;
};

class Analytics {
 public:
  explicit Analytics(AnalyticsConfig config = {}) : config_(config) {}

  // Process a fill report.
  void on_fill(const ExecutionReport& r, bool is_agent_taker);

  // Mark-to-market update using current mid price.
  void mark_to_market(SymbolID sym, double mid_price);

  // Accessors.
  double realized_pnl() const noexcept { return total_realized_; }
  double unrealized_pnl() const noexcept { return total_unrealized_; }
  double total_fees() const noexcept { return total_fees_; }
  double net_pnl() const noexcept {
    return total_realized_ + total_unrealized_ - total_fees_;
  }
  double max_drawdown() const noexcept { return max_drawdown_; }
  double sharpe_ratio() const noexcept;
  double sortino_ratio() const noexcept;
  double turnover() const noexcept { return total_turnover_; }

  // Print summary.
  void print(std::FILE* out = stdout) const;

  // Write PnL curve CSV.
  void write_pnl_csv(const std::string& path) const;

 private:
  AnalyticsConfig config_;
  double total_realized_ = 0.0;
  double total_unrealized_ = 0.0;
  double total_fees_ = 0.0;
  double total_turnover_ = 0.0;
  double peak_equity_ = 0.0;
  double max_drawdown_ = 0.0;
  double sum_returns_ = 0.0;
  double sum_sq_returns_ = 0.0;
  double sum_neg_sq_returns_ = 0.0;
  std::uint64_t return_count_ = 0;
  double last_equity_ = 0.0;
  double last_mid_ = 0.0;

  // Per-symbol state.
  struct SymState {
    std::int64_t inventory = 0;
    double avg_entry = 0.0;
    double realized = 0.0;
    double fees = 0.0;
    double last_mid = 0.0;
  };
  std::vector<SymState> symbols_;

  void ensure_symbol(SymbolID sym);
};

}  // namespace aster::strategy
