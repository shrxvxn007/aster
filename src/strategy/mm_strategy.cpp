// Aster — MM strategy implementation (Avellaneda-Stoikov).

#include "aster/strategy/mm_strategy.hpp"

#include <algorithm>
#include <cmath>

namespace aster::strategy {

MmStrategy::Quote MmStrategy::compute_quote(double mid_price,
                                            std::int64_t inventory,
                                            double time_remaining) const {
  Quote q;
  if (mid_price <= 0.0 || time_remaining <= 0.0) return q;

  const auto& p = params_;
  double sigma = p.gamma * p.sigma * p.sigma * time_remaining;
  // Reservation price: shift away from inventory direction.
  double reservation = mid_price - static_cast<double>(inventory) * sigma;

  // Optimal half-spread: gamma*sigma^2*T + (1/kappa)*ln(1 + gamma/kappa).
  double half_spread = p.base_spread +
                       0.5 * (sigma + (1.0 / p.kappa) * std::log(1.0 + p.gamma / p.kappa));

  // Inventory skew: widen spread on the overexposed side.
  double inventory_skew =
      std::clamp(static_cast<double>(inventory) / p.inventory_limit, -1.0, 1.0);
  double bid_half = half_spread * (1.0 + inventory_skew);
  double ask_half = half_spread * (1.0 - inventory_skew);

  double bid_price = reservation - bid_half;
  double ask_price = reservation + ask_half;

  // Snap to tick.
  double tick = p.tick_size;
  bid_price = std::floor(bid_price / tick) * tick;
  ask_price = std::ceil(ask_price / tick) * tick;

  if (bid_price > 0.0) {
    q.bid = from_double(bid_price);
  }
  if (ask_price > bid_price + tick) {
    q.ask = from_double(ask_price);
  }
  return q;
}

std::int64_t MmStrategy::inventory(SymbolID sym) const {
  if (sym >= inventory_.size()) return 0;
  return inventory_[static_cast<std::size_t>(sym)];
}

void MmStrategy::set_inventory(SymbolID sym, std::int64_t inv) {
  if (sym >= inventory_.size()) {
    inventory_.resize(static_cast<std::size_t>(sym) + 1, 0);
  }
  inventory_[static_cast<std::size_t>(sym)] = inv;
}

}  // namespace aster::strategy
