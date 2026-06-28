// Aster — Market-Making strategy (Avellaneda-Stoikov framework).
//
// Inventory-aware quoting: reservation price shifts based on current inventory,
// and spread widens as |inventory| grows. Quotes are placed at bid/ask around
// the reservation price.

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

  // Access inventory for a symbol.
  std::int64_t inventory(SymbolID sym) const;
  void set_inventory(SymbolID sym, std::int64_t inv);

  const MmParams& params() const { return params_; }

 private:
  MmParams params_;
  std::vector<std::int64_t> inventory_;
};

}  // namespace aster::strategy
