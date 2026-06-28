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
                                                          Timestamp ts) {
  Qty remaining = agg->qty_remaining;
  while (remaining > 0 && !lvl.empty()) {
    Order* maker = lvl.head();
    Qty fill_qty = remaining < maker->qty_remaining ? remaining
                                                    : maker->qty_remaining;
    // Emit two fill reports (taker + maker).
    // Note: designated initializers cannot be used with bitfields in C++20,
    // so we use positional aggregate initialization here.
    ExecutionReport tr{
        sym,                           // symbol
        {},                            // pad0
        agg->order_id,                 // order_id
        maker->order_id,               // counterparty
        maker->price,                  // price
        fill_qty,                      // qty
        {},                            // pad1
        agg->side,                     // side (bitfield)
        EventType::Fill,               // type (bitfield)
        {},                            // pad2
        {},                            // pad3
        ts,                            // timestamp
        ts,                            // recv_timestamp
    };
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
    callback_.on_fill(tr);
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
inline void MatchingEngine<Callback>::recompute_ask(SymbolID sym) {
  // With sorted per-side vectors this is a single front() read. Kept for
  // debug/invariance checks; no longer on the hot path.
  books_[sym].refresh_top();
}

template <typename Callback>
inline void MatchingEngine<Callback>::recompute_bid(SymbolID sym) {
  books_[sym].refresh_top();
}

template <typename Callback>
bool MatchingEngine<Callback>::add_order(OrderID id, SymbolID sym, Side side,
                                         Price price, Qty qty, Timestamp ts) {
  if (qty == 0 || sym >= books_.size()) return false;
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
Qty MatchingEngine<Callback>::execute_order_impl(Order* o, Qty qty, Timestamp ts) {
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
      lvl->pop_head();
      order_index_.erase(o->order_id);
      pool_.release(o);
      if (lvl->empty()) {
        book.erase_price(o->price, o->side);
        book.levels.erase(o->price);
        book.refresh_top();
      }
    }
  }
  return fill_qty;
}

template <typename Callback>
Qty MatchingEngine<Callback>::execute_order(OrderID id, Qty qty, Timestamp ts) {
  auto* p = order_index_.find(id);
  if (!p) return 0;
  return execute_order_impl(p->second, qty, ts);
}

template <typename Callback>
bool MatchingEngine<Callback>::cancel_order(OrderID id) {
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
bool MatchingEngine<Callback>::modify_order(OrderID id, Price new_price,
                                            Qty new_qty) {
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
