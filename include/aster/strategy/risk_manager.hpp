// Aster — RiskManager.
//
// Enforces trading risk limits on the market-making strategy:
//   1. Hard position limits: reject orders that would breach the limit.
//   2. Max-order throttle: limit the rate of new orders per symbol.
//   3. Drawdown kill-switch: halt trading when max drawdown exceeds a threshold.
//
// The RiskManager is consulted before every quote and after every fill. It is
// a pure policy object — it does not own the book or analytics, it only reads
// them via accessors passed to its methods.

#pragma once

#include "aster/core/types.hpp"

#include <cstdint>
#include <vector>

namespace aster::strategy {

struct RiskLimits {
  // Hard position limit (absolute value). Orders that would cause |inventory|
  // to exceed this are rejected.
  std::int64_t position_limit = 100;

  // Maximum number of new orders per symbol within the throttle window.
  std::uint32_t max_orders_per_window = 100;

  // Throttle window in nanoseconds.
  std::uint64_t throttle_window_ns = 1'000'000'000ULL;  // 1 second

  // Drawdown kill-switch: halt trading when max drawdown (in currency units)
  // exceeds this value. 0 = disabled.
  double max_drawdown = 0.0;
};

class RiskManager {
 public:
  explicit RiskManager(RiskLimits limits = {}) : limits_(limits) {}

  // Check if a new order is allowed given the current inventory and the
  // order's side and qty. Returns true if the order would not breach the
  // position limit.
  bool check_position_limit(SymbolID sym, Side side, Qty qty,
                            std::int64_t current_inventory);

  // Record a new order for throttling. Returns true if the order is within
  // the throttle limit.
  bool record_order(SymbolID sym, Timestamp now);

  // Check if the drawdown kill-switch is triggered. Returns true if trading
  // should continue, false if halted. Updates trading_allowed_ state.
  bool check_drawdown(double current_drawdown);

  // Check if trading is allowed (kill-switch not triggered).
  bool trading_allowed() const { return trading_allowed_; }

  // Reset the kill-switch (e.g., after manual intervention).
  void reset() { trading_allowed_ = true; }

  const RiskLimits& limits() const { return limits_; }

 private:
  RiskLimits limits_;
  bool trading_allowed_ = true;

  // Per-symbol order timestamps for throttling.
  struct OrderTimestamps {
    std::vector<Timestamp> timestamps;
  };
  std::vector<OrderTimestamps> order_history_;
};

}  // namespace aster::strategy
