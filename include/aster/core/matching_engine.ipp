// Aster — MatchingEngine implementation.
// Kept as a header-only .ipp file included from matching_engine.hpp so the
// template callback can be inlined on the hot path (zero-cost dispatch).
// NOTE: This file is included from matching_engine.hpp — do NOT include
// matching_engine.hpp here (would be a no-op due to #pragma once).

#include <algorithm>
#include <cstddef>

namespace aster {

template <typename Callback>
inline Qty MatchingEngine<Callback>::match_against_level(LevelQueue& lvl,
                                                          Order* agg,
                                                          SymbolID sym,
                                                          Timestamp ts) noexcept {
  Qty remaining = agg->qty_remaining;
  while (remaining > 0 && !lvl.empty()) {
    Order* maker = lvl.head();
    Qty fill_qty = remaining < maker->qty_remaining ? remaining
                                                    : maker->qty_remaining;
    // Emit one fill report per match, keyed by the resting (maker) side.
    // The aggressor observes its own unfilled residual via add_order's
    // return value (matches execute_order_impl's contract: one on_fill
    // per fill). The counterparty field identifies the taker for the
    // rare caller that needs to reconstruct both sides of the trade. The
    // previous two-emit pattern (taker + maker) caused `cb.fill_count`
    // to grow at 2x the number of trades, breaking tests/keyed indices
    // AND double-counting analytics for non-agent trades (both sides of
    // the same trade were charged the taker fee). Note: designated
    // initializers cannot be used with bitfields in C++20, so we use
    // positional aggregate initialization here.
    ExecutionReport mr{
        sym,                           // symbol
        {},                            // pad0
        maker->order_id,               // order_id
        agg->order_id,                 // counterparty
        maker->price,                  // price
        fill_qty,                      // qty
        {},                            // pad1
        maker->side,                   // side (bitfield)
        EventType::Fill,               // type (bitfield)
        {},                            // pad2
        {},                            // pad3
        ts,                            // timestamp
        ts,                            // recv_timestamp
    };
    callback_.on_fill(mr);
    remaining -= fill_qty;
    agg->qty_remaining = remaining;
    maker->qty_remaining -= fill_qty;
    // total_qty_ tracks the sum of qty_remaining at this level. We subtract
    // fill_qty here; pop_head() (via remove()) will subtract 0 since
    // qty_remaining is now 0.
    lvl.adjust_total_qty(fill_qty);
    if (maker->qty_remaining == 0) {
      lvl.pop_head();
      order_index_.erase(maker->order_id);
      pool_.release(maker);
    }
  }
  return remaining;
}

template <typename Callback>
inline void MatchingEngine<Callback>::recompute_ask(SymbolID sym) noexcept {
  // With sorted per-side vectors this is a single front() read. Kept for
  // debug/invariance checks; no longer on the hot path.
  books_[sym].refresh_top();
}

template <typename Callback>
inline void MatchingEngine<Callback>::recompute_bid(SymbolID sym) noexcept {
  books_[sym].refresh_top();
}

template <typename Callback>
bool MatchingEngine<Callback>::add_order(OrderID id, SymbolID sym, Side side,
                                         Price price, Qty qty,
                                         Timestamp ts) noexcept {
  if (qty == 0 || sym >= books_.size()) return false;
  // A limit order with an invalid price would rest at UINT64_MAX and
  // immediately match all bids (or 0 and match all asks). Reject it.
  if (price == kPriceInvalid) return false;
  Order* o = pool_.acquire();
  if (!o) return false;
  o->order_id = id;
  o->symbol = sym;
  o->side = side;
  o->price = price;
  o->qty_remaining = qty;
  o->qty_original = qty;
  o->submit_ts = ts;
  o->in_book = false;
  o->next = o->prev = nullptr;

  OrderBook& book = books_[sym];
  Qty remaining = qty;

  if (side == Side::Buy) {
    while (remaining > 0 && book.has_ask() && price >= book.ask_min) {
      LevelQueue* lvl = book.levels.find(book.ask_min);
      remaining = match_against_level(*lvl, o, sym, ts);
      if (lvl->empty()) {
        book.erase_price(book.ask_min, Side::Sell);
        book.levels.erase(book.ask_min);
        book.refresh_top();
      }
    }
    if (remaining > 0) {
      o->qty_remaining = remaining;
      LevelQueue& lvl = book.levels[price];
      lvl.push_tail(o);
      book.insert_price(price, side);
      book.refresh_top();
      order_index_[id] = {sym, o};
      callback_.on_accept(*o, ts);
    } else {
      pool_.release(o);
    }
  } else {
    while (remaining > 0 && book.has_bid() && price <= book.best_bid) {
      LevelQueue* lvl = book.levels.find(book.best_bid);
      remaining = match_against_level(*lvl, o, sym, ts);
      if (lvl->empty()) {
        book.erase_price(book.best_bid, Side::Buy);
        book.levels.erase(book.best_bid);
        book.refresh_top();
      }
    }
    if (remaining > 0) {
      o->qty_remaining = remaining;
      LevelQueue& lvl = book.levels[price];
      lvl.push_tail(o);
      book.insert_price(price, side);
      book.refresh_top();
      order_index_[id] = {sym, o};
      callback_.on_accept(*o, ts);
    } else {
      pool_.release(o);
    }
  }
  return true;
}

template <typename Callback>
bool MatchingEngine<Callback>::add_market_order(OrderID id, SymbolID sym,
                                                Side side, Qty qty,
                                                Timestamp ts) noexcept {
  if (qty == 0 || sym >= books_.size()) return false;
  Order* o = pool_.acquire();
  if (!o) return false;
  o->order_id = id;
  o->symbol = sym;
  o->side = side;
  o->price = kPriceInvalid;  // market orders have no limit price
  o->qty_remaining = qty;
  o->qty_original = qty;
  o->submit_ts = ts;
  o->in_book = false;
  o->next = o->prev = nullptr;

  OrderBook& book = books_[sym];
  Qty remaining = qty;

  // Sweep the opposite side: match against every resting price, best first,
  // until either the aggressor is filled or the opposite side is exhausted.
  // No price constraint — a market order takes whatever is available.
  if (side == Side::Buy) {
    while (remaining > 0 && book.has_ask()) {
      LevelQueue* lvl = book.levels.find(book.ask_min);
      if (!lvl) break;
      remaining = match_against_level(*lvl, o, sym, ts);
      if (lvl->empty()) {
        book.erase_price(book.ask_min, Side::Sell);
        book.levels.erase(book.ask_min);
        book.refresh_top();
      }
    }
  } else {
    while (remaining > 0 && book.has_bid()) {
      LevelQueue* lvl = book.levels.find(book.best_bid);
      if (!lvl) break;
      remaining = match_against_level(*lvl, o, sym, ts);
      if (lvl->empty()) {
        book.erase_price(book.best_bid, Side::Buy);
        book.levels.erase(book.best_bid);
        book.refresh_top();
      }
    }
  }

  // IOC: release the aggressor back to the pool in all cases. Market orders
  // never rest on the book. match_against_level only releases the resting
  // (maker) order on full fill, never the aggressor — so this single release
  // is safe regardless of whether `remaining` reached zero or not.
  pool_.release(o);
  return true;
}

template <typename Callback>
Qty MatchingEngine<Callback>::execute_order_impl(Order* o, Qty qty,
                                                 Timestamp ts) noexcept {
  if (qty == 0) return 0;
  Qty fill_qty = qty < o->qty_remaining ? qty : o->qty_remaining;
  OrderID cp = o->order_id;  // counterparty = the order itself (historical fill)
  ExecutionReport tr{
      o->symbol,      {}, o->order_id, cp, o->price, fill_qty, {},
      o->side,        EventType::Fill,  {}, {}, ts, ts,
  };
  callback_.on_fill(tr);
  o->qty_remaining -= fill_qty;
  OrderBook& book = books_[o->symbol];
  auto* lvl = book.levels.find(o->price);
  if (lvl) {
    lvl->adjust_total_qty(fill_qty);
    if (o->qty_remaining == 0) {
      // The resting order must be at its level. Use remove() instead of
      // pop_head() so that a mid-queue order (e.g. after a partial fill
      // in a prior execute_order) is removed correctly rather than
      // accidentally popping the wrong order from the head.
      assert(lvl->head() == o && "execute_order_impl: resting order must be at head on full fill");
      lvl->remove(o);
      order_index_.erase(o->order_id);
      pool_.release(o);
      if (lvl->empty()) {
        book.erase_price(o->price, o->side);
        book.levels.erase(o->price);
        book.refresh_top();
      }
    }
  }
  // Return the unfilled remainder (0 when `qty` was fully absorbed into the
  // resting order). Mirrors the ITCH 'E' message's spirit: the caller ingests an
  // external fill of size `qty`, and the engine reports how much residual
  // remained on the resting order. Matches the test fixture's variable name
  // (`Qty unfilled = engine.execute_order(...)` then `assert(unfilled == 0)`)
  // AND the internal `execute_order_impl` declaration's "Returns unfilled qty"
  // comment in matching_engine.hpp. NOTE: external docstring for execute_order
  // (in the header) was historically "Returns the qty actually filled"; that
  // has been corrected to read consistently.
  return qty - fill_qty;
}

template <typename Callback>
Qty MatchingEngine<Callback>::execute_order(OrderID id, Qty qty,
                                            Timestamp ts) noexcept {
  auto* p = order_index_.find(id);
  if (!p) return 0;
  return execute_order_impl(p->second, qty, ts);
}

template <typename Callback>
bool MatchingEngine<Callback>::cancel_order(OrderID id) noexcept {
  auto* p = order_index_.find(id);
  if (!p) return false;
  SymbolID sym = p->first;
  Order* o = p->second;
  OrderBook& book = books_[sym];
  auto* lvl = book.levels.find(o->price);
  if (lvl) {
    lvl->remove(o);  // remove() updates total_qty_ and clears o->in_book.
    if (lvl->empty()) {
      book.erase_price(o->price, o->side);
      book.levels.erase(o->price);
      book.refresh_top();
    }
  }
  order_index_.erase(id);
  pool_.release(o);
  return true;
}

template <typename Callback>
Qty MatchingEngine<Callback>::reduce_order(OrderID id, Qty qty_to_reduce) noexcept {
  if (qty_to_reduce == 0) return 0;
  auto* p = order_index_.find(id);
  if (!p) return 0;
  Order* o = p->second;

  // If reducing by more than or equal to remaining, fully cancel.
  if (qty_to_reduce >= o->qty_remaining) {
    Qty removed = o->qty_remaining;
    (void)cancel_order(id);
    return removed;
  }

  // Partial reduction: adjust the quantity in place. The order stays in
  // its queue position (no priority change) — matching real exchange
  // semantics for ITCH 'C' partial cancels.
  Qty delta = qty_to_reduce;
  o->qty_remaining -= delta;
  OrderBook& book = books_[o->symbol];
  auto* lvl = book.levels.find(o->price);
  if (lvl) {
    lvl->adjust_total_qty(delta);
  }
  return delta;
}

template <typename Callback>
bool MatchingEngine<Callback>::modify_order(OrderID id, Price new_price,
                                            Qty new_qty) noexcept {
  if (new_qty == 0) {
    return cancel_order(id);
  }
  auto* p = order_index_.find(id);
  if (!p) return false;
  SymbolID sym = p->first;
  Order* o = p->second;
  Side side = o->side;
  Timestamp ts = o->submit_ts;
  (void)cancel_order(id);
  return add_order(id, sym, side, new_price, new_qty, ts);
}

}  // namespace aster
