// Aster — cold-path type helpers.
//
// Implements to_price/from_double out of line so that every TU including
// core headers doesn't need to compile the (trivial but symbol-heavy) math.

#include "aster/core/types.hpp"

#include <cstdint>

namespace aster {

Price from_double(double v) {
  return static_cast<Price>(v * static_cast<double>(kPriceScale) + 0.5);
}

double to_price(Price p) {
  return static_cast<double>(p) / static_cast<double>(kPriceScale);
}

}  // namespace aster
