// Aster — per-symbol OrderBook.
//
// Two flat hash maps:
//   levels_: Price -> LevelQueue  (price-time priority)
//   lookup_: OrderID -> Order*    (O(1) cancel/modify)
//
// best_bid / ask_min are cached top-of-book prices, updated incrementally.
// alignas(64) so adjacent symbols don't share a cache line.

#pragma once

#include "level_queue.hpp"
#include "order.hpp"
#include "order_pool.hpp"

#include "aster/utils/flat_hash_map.h"
#include "aster/core/types.hpp"

#include <cstdint>

namespace aster {

struct alignas(64) OrderBook {
  flat_hash_map<Price, LevelQueue> levels;
  flat_hash_map<OrderID, Order*> lookup;
  Price best_bid = 0;               // 0 = no bids
  Price ask_min = kPriceInvalid;    // UINT64_MAX = no asks

  // Padding to fill the cache line (64 bytes) after the two pointers + 2 prices.
  // levels and lookup are heap-allocated vectors; only the map headers live here.
  // Header sizes: each flat_hash_map has 3 vectors (keys, vals, meta) + 2 size_t
  // + 2 functors. Roughly 3*3*8 + 16 + 16 = 104 bytes each. So this struct is
  // much larger than 64 bytes. The alignas(64) still helps: the start of each
  // book is cache-line aligned, and the hot fields (best_bid, ask_min) are at
  // known offsets. We don't strictly need to pad to 64 bytes.

  bool has_bid() const noexcept { return best_bid != 0; }
  bool has_ask() const noexcept { return ask_min != kPriceInvalid; }

  // Returns true if the book is internally consistent (no crossed market).
  bool is_consistent() const noexcept {
    return !(has_bid() && has_ask() && best_bid >= ask_min);
  }
};

}  // namespace aster
