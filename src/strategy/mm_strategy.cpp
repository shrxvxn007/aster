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

  // Snap to tick using integer arithmetic on the fixed-point Price space.
  // Prices are scaled 1e5 (kPriceScale); tick_size is a double in price
  // space. We convert both bid/ask and tick to integer Price units and snap
  // there to avoid double-rounding differences across compilers or -ffast-math.
  Price bid_px = from_double(bid_price);
  Price ask_px = from_double(ask_price);
  Price tick_px = from_double(p.tick_size);
  if (tick_px > 0) {
    // Snap bid down (floor in Price space).
    bid_px = (bid_px / tick_px) * tick_px;
    // Snap ask up (ceil in Price space).
    ask_px = ((ask_px + tick_px - 1) / tick_px) * tick_px;
  }

  if (bid_px > 0) {
    q.bid = bid_px;
  }
  if (ask_px > bid_px + tick_px) {
    q.ask = ask_px;
  }
  return q;
}

double MmStrategy::toxicity_spread_multiplier(double toxic_ratio,
                                               double max_toxic_penalty) {
  if (toxic_ratio <= 0.0) return 1.0;
  if (toxic_ratio > 1.0) toxic_ratio = 1.0;
  // Linear penalty: at 100% toxic, spread widens by (1 + max_toxic_penalty).
  return 1.0 + toxic_ratio * max_toxic_penalty;
}

double MmStrategy::fill_probability(double volume_ahead, double lambda,
                                     double horizon_s) {
  // Degenerate cases.
  if (volume_ahead <= 0.0) return 1.0;  // already at front → fill imminent.
  if (lambda <= 0.0 || horizon_s <= 0.0) return 0.0;  // no arrival process.

  // Poisson mean over the lookahead horizon.
  double mean = lambda * horizon_s;
  // We want P(N >= volume_ahead) where N ~ Poisson(mean).
  // = 1 - P(N <= volume_ahead - 1) = 1 - CDF(k = volume_ahead - 1).
  // Sum the PMF from 0 to floor(volume_ahead - 1).
  int k_max = static_cast<int>(std::floor(volume_ahead - 1.0));
  if (k_max < 0) k_max = 0;

  // Accumulate CDF via the iterative Poisson recurrence:
  //   P(N = 0) = exp(-mean)
  //   P(N = i) = P(N = i-1) * mean / i
  // This avoids large factorials and is numerically stable for the small
  // means typical over a short horizon.
  double cdf = 0.0;
  double term = std::exp(-mean);  // P(N = 0)
  for (int i = 0; i <= k_max; ++i) {
    cdf += term;
    term *= mean / static_cast<double>(i + 1);
  }
  double p = 1.0 - cdf;
  if (p < 0.0) p = 0.0;
  if (p > 1.0) p = 1.0;
  return p;
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
