// Aster — MatchingEngine.
//
// Multi-symbol, price-time priority limit order book.
// Template on Callback for zero-cost dispatch of ExecutionReports.
//
// Critical path (add_order) is allocation-free: all Order nodes come from the
// pre-allocated pool; all map operations are flat-hash O(1) amortized.

#pragma once

#include "order_book.hpp"
#include "order_pool.hpp"

#include "aster/core/types.hpp"
#include "aster/utils/flat_hash_map.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace aster {

// Callback interface the engine invokes on events. The concrete type must
// provide on_fill(const ExecutionReport&) and on_accept(const Order&, Timestamp).
// Templated for zero-cost dispatch.
template <typename C>
concept EngineCallback = requires(C cb, const ExecutionReport& r, const Order& o,
                                 Timestamp ts) {
  cb.on_fill(r);
  cb.on_accept(o, ts);
};

// The concept is enforced via static_assert in the constructor body (not as a
// template constraint, and not at class-scope) so that incomplete callback
// types — e.g. Backtest& used as a member inside Backtest itself — can be
// named without triggering an immediate constraint check. The assert fires
// only when the constructor is instantiated, by which time the callback type
// is complete.
template <typename Callback>
class MatchingEngine {
 public:
  MatchingEngine(std::uint32_t num_symbols, std::uint32_t pool_size,
                 Callback cb)
      : books_(num_symbols),
        pool_(pool_size),
        callback_(cb),
        order_index_() {
    // Fires only on instantiation, when Callback is complete.
    static_assert(EngineCallback<Callback>,
                  "Callback must satisfy EngineCallback: provide "
                  "on_fill(const ExecutionReport&) and "
                  "on_accept(const Order&, Timestamp)");
    order_index_.reserve(pool_size);
  }

  // Adds an order. Matches against the opposite side; rests any residual.
  // Returns false if the pool is exhausted or the order is invalid.
  [[nodiscard]] bool add_order(OrderID id, SymbolID sym, Side side, Price price,
                               Qty qty, Timestamp ts);

  // Cancels an order by ID. O(1) via lookup map + intrusive list splice.
  [[nodiscard]] bool cancel_order(OrderID id);

  // Modifies an order. Cancel + re-add semantics: loses queue priority.
  [[nodiscard]] bool modify_order(OrderID id, Price new_price, Qty new_qty);

  // Historical execution: removes `qty` from the resting order `id`. If the fill
  // completes the order, the order is removed from the book and pool. Returns
  // the qty actually filled. Returns 0 if the order is not in the book.
  [[nodiscard]] Qty execute_order(OrderID id, Qty qty, Timestamp ts);

  Price best_bid(SymbolID s) const {
    if (s >= books_.size()) return 0;
    return books_[s].best_bid;
  }
  Price best_ask(SymbolID s) const {
    if (s >= books_.size()) return kPriceInvalid;
    return books_[s].ask_min;
  }
  Qty level_qty(SymbolID s, Price p) const {
    if (s >= books_.size()) return 0;
    auto* lvl = books_[s].levels.find(p);
    return lvl ? lvl->total_qty() : 0;
  }
  const Order* find_order(OrderID id) const {
    return const_cast<MatchingEngine*>(this)->find_order(id);
  }

  // Accessors for strategy.
  const std::vector<OrderBook>& books() const { return books_; }
  std::vector<OrderBook>& books() { return books_; }
  OrderPool& pool() { return pool_; }

  // Find an order by ID (non-const, for internal use).
  Order* find_order(OrderID id) {
    auto* p = order_index_.find(id);
    return p ? p->second : nullptr;
  }

  // Execute against a resting order. Returns unfilled qty (0 if fully filled).
  Qty execute_order_impl(Order* o, Qty qty, Timestamp ts);

 private:
  // Match `agg` (aggressor) against the FIFO queue at one price level.
  // Returns the aggressor's remaining qty.
  Qty match_against_level(LevelQueue& lvl, Order* agg, SymbolID sym,
                          Timestamp ts);

  void recompute_ask(SymbolID sym);
  void recompute_bid(SymbolID sym);

  std::vector<OrderBook> books_;
  OrderPool pool_;
  Callback callback_;
  // Global order index: OrderID -> (SymbolID, Order*). Enables O(1) cancel
  // without scanning every book.
  flat_hash_map<OrderID, std::pair<SymbolID, Order*>> order_index_;
};

}  // namespace aster
