// Aster — QueueTracker.
//
// For each agent-owned passive order, tracks `volume_ahead` at its price level.
// On incoming Cancel/Execute events at that (symbol, price), decrements the
// agent's ahead-volume deterministically.
//
// The per-(symbol, price) index makes on_level_event O(k) where k = agent
// orders at that level (typically ≤ 2), instead of O(N) where N = all agent
// orders across the book.

#pragma once

#include "aster/core/types.hpp"
#include "aster/utils/flat_hash_map.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace aster::strategy {

struct AgentOrder {
  SymbolID symbol;
  Price price;
  Qty volume_ahead;  // total qty ahead of us at this price
  Qty our_qty;
};

class QueueTracker {
 public:
  // Register a new agent order. Price MUST fit in 48 bits for level_key()
  // to disambiguate (sym, price) — above 2^48 the SymbolID nibble is
  // corrupted silently. We assert explicitly so the failure mode is a
  // clean abort at registration rather than a confused tracker later.
  void on_agent_order(OrderID id, SymbolID sym, Price price, Qty qty,
                      Qty volume_ahead) {
    assert(price < (Price(1) << 48) &&
           "QueueTracker: price exceeds 48-bit packing bound (≈ $281T at "
           "1e5 scale). level_key() would clobber the SymbolID nibble.");
    agents_[id] = {sym, price, volume_ahead, qty};
    level_index_[level_key(sym, price)].push_back(id);
  }

  // Called when a market event (cancel or execute) removes qty at a level.
  void on_level_event(SymbolID sym, Price price, Qty removed_qty) {
    std::uint64_t key = level_key(sym, price);
    auto* ids = level_index_.find(key);
    if (!ids) return;
    Qty remaining = removed_qty;
    // Walk only the agent orders that sit at this (symbol, price).
    for (auto it = ids->begin(); it != ids->end() && remaining > 0;) {
      OrderID id = *it;
      auto* ao = agents_.find(id);
      if (!ao) {
        // Order already gone (filled/cancelled); drop from index.
        it = ids->erase(it);
        continue;
      }
      Qty dec = remaining < ao->volume_ahead ? remaining : ao->volume_ahead;
      ao->volume_ahead -= dec;
      remaining -= dec;
      ++it;
    }
    if (ids->empty()) level_index_.erase(key);
  }

  // Remove an agent order (filled or cancelled).
  void remove(OrderID id) {
    auto* ao = agents_.find(id);
    if (!ao) return;
    // Erase from level_index_ so we don't walk dead IDs later.
    std::uint64_t key = level_key(ao->symbol, ao->price);
    auto* ids = level_index_.find(key);
    if (ids) {
      auto it = std::find(ids->begin(), ids->end(), id);
      if (it != ids->end()) ids->erase(it);
      if (ids->empty()) level_index_.erase(key);
    }
    agents_.erase(id);
  }

  const AgentOrder* find(OrderID id) const {
    return const_cast<QueueTracker*>(this)->agents_.find(id);
  }

 private:
  // Combine (SymbolID, Price) into a single 64-bit key for the index.
  // Price lives in the high 48 bits; SymbolID in the low 16. ASSUMES Price
  // fits in 48 bits (fixed-point 1e5 scale → max ≈ 2.8e14 < 2^48).
  // on_agent_order() asserts this; this is the only entry point that adds
  // to level_index_, so an assertion there is sufficient coverage.
  static std::uint64_t level_key(SymbolID sym, Price price) noexcept {
    return (static_cast<std::uint64_t>(price) << 16) |
           static_cast<std::uint64_t>(sym);
  }

  flat_hash_map<OrderID, AgentOrder> agents_;
  flat_hash_map<std::uint64_t, std::vector<OrderID>> level_index_;
};

}  // namespace aster::strategy
