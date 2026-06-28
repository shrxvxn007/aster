// Aster — Analytics.
//
// Tracks per-symbol inventory, realized/unrealized PnL, fees, equity curve,
// max drawdown, Sharpe, Sortino, turnover. Updated incrementally per fill.

#pragma once

#include "aster/core/types.hpp"

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <deque>
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

  // Process a fill report. `ts` is the fill timestamp (used for adverse
  // selection lookback windowing).
  void on_fill(const ExecutionReport& r, bool is_agent_taker,
               Timestamp ts = 0);

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

  // Adverse selection: fraction of fills that were "toxic" (mid moved
  // against the position within the lookback window).
  double toxic_fill_ratio() const noexcept {
    return total_fills_ > 0
               ? static_cast<double>(toxic_fills_) /
                     static_cast<double>(total_fills_)
               : 0.0;
  }
  double avg_toxic_cost() const noexcept {
    return toxic_fills_ > 0 ? toxic_cost_ / static_cast<double>(toxic_fills_)
                            : 0.0;
  }
  std::uint64_t total_fills() const noexcept { return total_fills_; }
  std::uint64_t fill_events() const noexcept { return total_fills_; }

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

  // Adverse selection tracking.
  std::uint64_t total_fills_ = 0;
  std::uint64_t toxic_fills_ = 0;
  double toxic_cost_ = 0.0;
  // Recent fills awaiting a mid update to classify. Traded off after the
  // lookback window expires.
  struct PendingFill {
    SymbolID symbol;
    Timestamp fill_ts;
    double fill_price;
    Side side;         // agent's side on this fill
    std::int64_t qty;  // signed: +buy, -sell
  };
  std::deque<PendingFill> pending_fills_;
  static constexpr std::uint64_t kToxicLookbackNs = 100'000'000ULL;  // 100 ms

  // Per-symbol state.
  struct SymState {
    std::int64_t inventory = 0;
    double avg_entry = 0.0;
    double realized = 0.0;
    double fees = 0.0;
    double last_mid = 0.0;
    double tick_size = 0.0001;  // minimum price increment for toxic check
  };
  std::vector<SymState> symbols_;

  void ensure_symbol(SymbolID sym);
  void classify_pending(Timestamp now);
};

}  // namespace aster::strategy
