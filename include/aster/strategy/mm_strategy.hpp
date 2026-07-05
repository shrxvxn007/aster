// Aster — Market-Making strategy (Avellaneda-Stoikov framework).
//
// Inventory-aware quoting: reservation price shifts based on current inventory,
// and spread widens as |inventory| grows. Quotes are placed at bid/ask around
// the reservation price.
//
// Fill-probability estimation models the market-order arrival stream as a
// Poisson process with an online exponential-moving-average rate. Given a
// queue position (volume_ahead) and a lookahead horizon, the probability that
// our order fills is Poisson CDF: P(≥ volume_ahead shares arrive in horizon).

#pragma once

#include "aster/core/types.hpp"

#include <cstdint>
#include <vector>

namespace aster::strategy {

struct MmParams {
  double gamma = 0.1;           // risk aversion
  double sigma = 0.02;          // volatility estimate (per-second)
  double T = 1.0;               // time horizon (seconds)
  double kappa = 1.0;           // order book liquidity
  double base_spread = 0.01;    // minimum half-spread
  double tick_size = 0.0001;    // price increment
  double inventory_limit = 100; // max position before skewing hard
  Qty base_lot = 1;             // quote size
};

class MmStrategy {
 public:
  explicit MmStrategy(MmParams params = {}) : params_(params) {}

  // Compute bid/ask prices given current mid and inventory.
  // Returns {bid_price, ask_price}. If a side should not be quoted, returns
  // kPriceInvalid for that side.
  struct Quote {
    Price bid = kPriceInvalid;
    Price ask = kPriceInvalid;
  };

  Quote compute_quote(double mid_price, std::int64_t inventory,
                      double time_remaining) const;

  // Compute a toxicity-adjusted half-spread multiplier. When the recent toxic
  // fill ratio is high (adverse selection detected), the strategy widens its
  // spread to compensate for the cost of being adversely selected. Returns a
  // multiplier >= 1.0 applied to the base half-spread.
  //
  //   toxic_ratio in [0, 1]: fraction of recent fills that were toxic.
  //   The multiplier grows linearly from 1.0 (no toxicity) to
  //   1.0 + max_toxic_penalty (fully toxic).
  static double toxicity_spread_multiplier(double toxic_ratio,
                                           double max_toxic_penalty = 2.0);

  // Estimate the fill probability for an order with `volume_ahead` shares in
  // front of it, over a lookahead window of `horizon_s` seconds. Models the
  // market-order arrival rate as `lambda` shares/second. Returns a value
  // in [0, 1].
  //
  // Poisson model: arrivals over horizon ~ Poisson(lambda * horizon).
  // P(fill) = P(arrivals >= volume_ahead) = 1 - CDF(volume_ahead - 1).
  static double fill_probability(double volume_ahead, double lambda,
                                 double horizon_s);

  // Access inventory for a symbol.
  std::int64_t inventory(SymbolID sym) const;
  void set_inventory(SymbolID sym, std::int64_t inv);

  const MmParams& params() const { return params_; }

 private:
  MmParams params_;
  std::vector<std::int64_t> inventory_;
};

}  // namespace aster::strategy
