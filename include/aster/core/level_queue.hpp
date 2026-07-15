// Aster — LevelQueue.
//
// Intrusive doubly-linked list of Orders at a single price level, FIFO
// priority (pop from head, push to tail). O(1) for push/pop/remove.
// total_qty is maintained incrementally — used by the queue tracker to compute
// volume-ahead deterministically.

#pragma once

#include "order.hpp"

#include "aster/core/types.hpp"

#include <cassert>

namespace aster {

class LevelQueue {
 public:
  [[nodiscard]] Order* head() noexcept { return head_; }
  [[nodiscard]] Order* tail() noexcept { return tail_; }
  [[nodiscard]] const Order* head() const noexcept { return head_; }
  [[nodiscard]] const Order* tail() const noexcept { return tail_; }
  [[nodiscard]] Qty total_qty() const noexcept { return total_qty_; }
  [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

  // Direct adjust of total_qty_ by the matching engine after a partial fill.
  void adjust_total_qty(Qty delta) noexcept { total_qty_ -= delta; }

  // Snap-to replacement of total_qty_. Used by the L2 replay path
  // (Backtest::handle_l2) to commit the level's aggregate to the snapshot
  // value carried in an L2AggregateMsg. Hot-path matching semantics are
  // unaffected because match_against_level walks head→tail in qty_remaining
  // order; total_qty_ is a cached view consumed by level_qty() and by the
  // QueueTracker's per-(symbol, price) volume-ahead estimation. We do not
  // touch head_/tail_ — an L2 snapshot overrides the cache, not the order
  // chain. Pairs naturally with adjust_total_qty (subtract): the latter is
  // the L3 incremental update, this is the L2 wholesale overwrite.
  void override_total_qty(Qty qty) noexcept { total_qty_ = qty; }

  // Append to the tail — new order gets lowest priority at this price.
  void push_tail(Order* o) noexcept {
    assert(o != nullptr);
    o->prev = tail_;
    o->next = nullptr;
    if (tail_) {
      tail_->next = o;
    } else {
      head_ = o;
    }
    tail_ = o;
    total_qty_ += o->qty_remaining;
    o->in_book = true;
  }

  // Remove from the head — highest priority order at this price.
  Order* pop_head() noexcept {
    Order* o = head_;
    if (!o) return nullptr;
    remove(o);
    return o;
  }

  // O(1) splice using the embedded prev/next pointers.
  // Decrements total_qty_ by o->qty_remaining. Caller must ensure that if
  // qty_remaining was previously reduced (partial fill), the caller has
  // already adjusted total_qty_ via adjust_total_qty() to account for the
  // reduced qty_remaining — otherwise pass an order whose qty_remaining is
  // still accurate and this function will update correctly.
  void remove(Order* o) noexcept {
    assert(o != nullptr);
    total_qty_ -= o->qty_remaining;
    if (o->prev) {
      o->prev->next = o->next;
    } else {
      head_ = o->next;
    }
    if (o->next) {
      o->next->prev = o->prev;
    } else {
      tail_ = o->prev;
    }
    o->next = o->prev = nullptr;
    o->in_book = false;
  }

  void clear() noexcept {
    head_ = tail_ = nullptr;
    total_qty_ = 0;
  }

 private:
  Order* head_ = nullptr;
  Order* tail_ = nullptr;
  // Total qty of all orders in this queue (sum of qty_remaining). Maintained
  // incrementally by push_tail/remove and direct adjustment by the match loop.
  Qty total_qty_ = 0;
};

}  // namespace aster
