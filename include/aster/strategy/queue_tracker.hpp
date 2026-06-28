// Aster — QueueTracker.
//
// For each agent-owned passive order, tracks `volume_ahead` at its price level.
// On incoming Cancel/Execute events at that (symbol, price), decrements the
// agent's ahead-volume deterministically.

#pragma once

#include "aster/core/types.hpp"
#include "aster/utils/flat_hash_map.h"

#include <cstdint>

namespace aster::strategy {

struct AgentOrder {
  SymbolID symbol;
  Price price;
  Qty volume_ahead;  // total qty ahead of us at this price
  Qty our_qty;
};

class QueueTracker {
 public:
  // Register a new agent order.
  void on_agent_order(OrderID id, SymbolID sym, Price price, Qty qty,
                      Qty volume_ahead) {
    agents_[id] = {sym, price, volume_ahead, qty};
  }

  // Called when a market event (cancel or execute) removes qty at a level.
  void on_level_event(SymbolID sym, Price price, Qty removed_qty) {
    Qty remaining = removed_qty;
    agents_.for_each([&](const OrderID& id, AgentOrder& ao) {
      if (remaining == 0) return;
      if (ao.symbol == sym && ao.price == price) {
        Qty dec = remaining < ao.volume_ahead ? remaining : ao.volume_ahead;
        ao.volume_ahead -= dec;
        remaining -= dec;
      }
    });
  }

  // Remove an agent order (filled or cancelled).
  void remove(OrderID id) { agents_.erase(id); }

  const AgentOrder* find(OrderID id) const {
    return const_cast<QueueTracker*>(this)->agents_.find(id);
  }

 private:
  flat_hash_map<OrderID, AgentOrder> agents_;
};

}  // namespace aster::strategy
