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

#include <algorithm>
#include <cstdint>
#include <vector>

namespace aster {

struct alignas(64) OrderBook {
  flat_hash_map<Price, LevelQueue> levels;
  flat_hash_map<OrderID, Order*> lookup;
  Price best_bid = 0;               // 0 = no bids
  Price ask_min = kPriceInvalid;    // UINT64_MAX = no asks

  // Sorted price lists per side. Kept in sync with `levels` so that best_bid /
  // ask_min can be read in O(1) without scanning the flat-map's probe array.
  //   bids: descending (largest first) → best_bid = bids.front()
  //   asks: ascending  (smallest first) → ask_min = asks.front()
  // Levels count is typically <200, so linear-scan insert/erase is fine.
  std::vector<Price> bids;
  std::vector<Price> asks;

  bool has_bid() const noexcept { return !bids.empty(); }
  bool has_ask() const noexcept { return !asks.empty(); }

  // Returns true if the book is internally consistent (no crossed market).
  bool is_consistent() const noexcept {
    return !(has_bid() && has_ask() && bids.front() >= asks.front());
  }

  // Insert a price into the appropriate sorted side. No-op if already present.
  void insert_price(Price price, Side side) {
    if (side == Side::Buy) {
      auto it = std::lower_bound(bids.begin(), bids.end(), price,
                                 std::greater<Price>{});
      if (it != bids.end() && *it == price) return;
      bids.insert(it, price);
    } else {
      auto it = std::lower_bound(asks.begin(), asks.end(), price);
      if (it != asks.end() && *it == price) return;
      asks.insert(it, price);
    }
  }

  // Remove a price from the appropriate sorted side.
  void erase_price(Price price, Side side) {
    if (side == Side::Buy) {
      auto it = std::lower_bound(bids.begin(), bids.end(), price,
                                 std::greater<Price>{});
      if (it != bids.end() && *it == price) bids.erase(it);
    } else {
      auto it = std::lower_bound(asks.begin(), asks.end(), price);
      if (it != asks.end() && *it == price) asks.erase(it);
    }
  }

  // Recompute best_bid / ask_min from the sorted vectors. Cheap: just reads
  // the front element. Kept as a single source of truth.
  void refresh_top() noexcept {
    best_bid = bids.empty() ? 0 : bids.front();
    ask_min = asks.empty() ? kPriceInvalid : asks.front();
  }
};

}  // namespace aster
