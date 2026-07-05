// Aster — RiskManager implementation.

#include "aster/strategy/risk_manager.hpp"

#include <algorithm>
#include <cassert>

namespace aster::strategy {

bool RiskManager::check_position_limit(SymbolID /*sym*/, Side side, Qty qty,
                                       std::int64_t current_inventory) {
  // Compute the projected inventory after this order fills.
  std::int64_t delta = (side == Side::Buy) ? static_cast<std::int64_t>(qty)
                                            : -static_cast<std::int64_t>(qty);
  std::int64_t projected = current_inventory + delta;
  // Check against position limit (ignore throttle/kill-switch for this check).
  return std::abs(projected) <= limits_.position_limit;
}

bool RiskManager::record_order(SymbolID sym, Timestamp now) {
  if (sym >= order_history_.size()) {
    order_history_.resize(static_cast<std::size_t>(sym) + 1);
  }
  auto& history = order_history_[sym];

  // Remove timestamps outside the throttle window.
  Timestamp window_start = now - limits_.throttle_window_ns;
  auto it = std::upper_bound(history.timestamps.begin(),
                             history.timestamps.end(), window_start);
  history.timestamps.erase(history.timestamps.begin(), it);

  // Check if we're within the throttle limit.
  if (history.timestamps.size() >= limits_.max_orders_per_window) {
    return false;  // throttle exceeded
  }

  // Record the order.
  history.timestamps.push_back(now);
  return true;
}

bool RiskManager::check_drawdown(double current_drawdown) {
  // If kill-switch already triggered, stay halted.
  if (!trading_allowed_) return false;
  // If drawdown limit is set and breached, trigger kill-switch.
  if (limits_.max_drawdown > 0.0 && current_drawdown >= limits_.max_drawdown) {
    trading_allowed_ = false;
    return false;
  }
  return true;
}

}  // namespace aster::strategy
