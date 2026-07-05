// Aster — engine tests.
//
// Exercises the matching engine's price-time priority, partial fills, partial
// cancels, multi-symbol independence, market-order IOC behaviour, and the
// cache-friendly ExecutionReport layout. Uses <cassert> only — no GTest
// dependency, so the suite compiles with the same flags as the rest of the
// project and runs under ctest in seconds.
//
// All [[nodiscard]] returns are either captured into a variable or asserted
// directly so the suite builds cleanly under -Wunused-result.

#include "aster/core/matching_engine.hpp"
#include "aster/core/order.hpp"
#include "aster/core/types.hpp"

// MatchingEngine is a template — its body lives in matching_engine.ipp.
// Every TU that instantiates it on a concrete callback type must include
// this header after the callback's full definition. This mirrors what
// src/sim.cpp and src/strategy/backtest.cpp do.
#include "aster/core/matching_engine.ipp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace aster;

namespace {

// Minimal callback: pushes fills into a std::vector the test can inspect.
// Records every accept as well so we can verify the engine notifies the
// callback on every state transition.
struct CaptureCallback {
  std::vector<ExecutionReport> fills;
  std::vector<std::pair<OrderID, Qty>> accepts;  // (id, qty_remaining)
  std::uint64_t fill_count = 0;
  std::uint64_t accept_count = 0;

  void on_fill(const ExecutionReport& r) {
    ++fill_count;
    fills.push_back(r);
  }
  void on_accept(const Order& o, Timestamp) {
    ++accept_count;
    accepts.emplace_back(o.order_id, o.qty_remaining);
  }
};

void test_execution_report_layout() {
  // Compile-time guarantee: ExecutionReport is exactly one cache line.
  static_assert(sizeof(ExecutionReport) == 64,
                "ExecutionReport must pack into one cache line");
  static_assert(alignof(ExecutionReport) >= 8,
                "ExecutionReport must be at least 8-byte aligned");
  std::printf("[ok] test_execution_report_layout\n");
}

void test_empty_book() {
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(/*num_symbols=*/2, /*pool=*/1024, cb);

  // No orders yet: best_bid=0, best_ask=kPriceInvalid.
  assert(engine.best_bid(0) == 0);
  assert(engine.best_ask(0) == kPriceInvalid);
  assert(engine.level_qty(1, 100ULL) == 0);
  // is_consistent() holds trivially on an empty book.
  assert(engine.books()[0].is_consistent());
  std::printf("[ok] test_empty_book\n");
}

void test_add_no_match() {
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(1, 1024, cb);

  // Place a passive bid below the ask side — no match expected.
  bool ok = engine.add_order(/*id=*/1, /*sym=*/0, Side::Buy,
                             /*price=*/1'000'000ULL /* 10.00000 */,
                             /*qty=*/5, /*ts=*/100);
  assert(ok);
  assert(cb.accept_count == 1);
  assert(cb.fill_count == 0);

  ok = engine.add_order(/*id=*/2, /*sym=*/0, Side::Sell, 2'000'000ULL, 7, 200);
  assert(ok);
  assert(cb.accept_count == 2);
  assert(cb.fill_count == 0);

  assert(engine.best_bid(0) == 1'000'000ULL);
  assert(engine.best_ask(0) == 2'000'000ULL);
  assert(engine.level_qty(0, 1'000'000ULL) == 5);
  assert(engine.level_qty(0, 2'000'000ULL) == 7);
  assert(engine.books()[0].is_consistent());
  std::printf("[ok] test_add_no_match\n");
}

void test_price_time_priority() {
  // Two bids at the same price — the EARLIER bid must fill first.
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(1, 1024, cb);

  assert(engine.add_order(1, 0, Side::Buy, 100ULL, 5, 1));
  assert(engine.add_order(2, 0, Side::Buy, 100ULL, 5, 2));
  // Aggressive ask lifts 6 shares — first bid (id=1) drains to 0, second fills 1.
  assert(engine.add_order(3, 0, Side::Sell, 100ULL, 6, 3));

  assert(cb.fill_count == 2);
  assert(cb.fills[0].order_id == 1);  // first bid fills first
  assert(cb.fills[0].qty == 5);
  assert(cb.fills[1].order_id == 2);
  assert(cb.fills[1].qty == 1);

  // The survivor is id=2 with qty_remaining=4 (eligible to be picked next).
  const Order* survivor = engine.find_order(2);
  assert(survivor != nullptr);
  assert(survivor->qty_remaining == 4);

  // Order id=1 must be gone from the book (zero remaining → removed).
  assert(engine.find_order(1) == nullptr);
  // 4 shares remain at price 100.
  assert(engine.level_qty(0, 100ULL) == 4);
  assert(engine.best_bid(0) == 100ULL);
  std::printf("[ok] test_price_time_priority\n");
}

void test_partial_fill() {
  // Cross between an ask and two bids where neither single side covers the
  // entire aggressor — exercises the partial-fill path on both ends.
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(1, 1024, cb);

  assert(engine.add_order(1, 0, Side::Buy, 100ULL, 6, 1));
  assert(engine.add_order(2, 0, Side::Buy, 100ULL, 6, 2));
  // Sell 10 at 100 — fully lifts id=1 (6), then 4 of id=2.
  assert(engine.add_order(3, 0, Side::Sell, 100ULL, 10, 3));

  assert(cb.fill_count == 2);
  assert(cb.fills[0].order_id == 1 && cb.fills[0].qty == 6);
  assert(cb.fills[1].order_id == 2 && cb.fills[1].qty == 4);
  assert(engine.find_order(1) == nullptr);
  assert(engine.find_order(2) != nullptr);
  assert(engine.find_order(2)->qty_remaining == 2);
  assert(engine.level_qty(0, 100ULL) == 2);
  std::printf("[ok] test_partial_fill\n");
}

void test_cancel_fully_removes() {
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(1, 1024, cb);

  assert(engine.add_order(1, 0, Side::Buy, 100ULL, 5, 1));
  assert(engine.best_bid(0) == 100ULL);
  assert(engine.cancel_order(1));
  assert(engine.find_order(1) == nullptr);
  assert(engine.best_bid(0) == 0);
  assert(engine.level_qty(0, 100ULL) == 0);
  assert(engine.books()[0].is_consistent());
  std::printf("[ok] test_cancel_fully_removes\n");
}

void test_reduce_order_partial_cancel() {
  // ITCH 'C' partial cancel — qty_to_reduce < remaining.
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(1, 1024, cb);

  assert(engine.add_order(1, 0, Side::Buy, 100ULL, 10, 1));
  Qty removed = engine.reduce_order(1, /*qty_to_reduce=*/3);
  assert(removed == 3);

  const Order* o = engine.find_order(1);
  assert(o != nullptr);
  assert(o->qty_remaining == 7);  // 10 - 3
  assert(engine.level_qty(0, 100ULL) == 7);  // level qty tracks order

  // Reducing by > remaining fully cancels the order.
  removed = engine.reduce_order(1, /*qty_to_reduce=*/1000);
  assert(removed == 7);
  assert(engine.find_order(1) == nullptr);
  assert(engine.level_qty(0, 100ULL) == 0);
  std::printf("[ok] test_reduce_order_partial_cancel\n");
}

void test_modified_order_loses_priority() {
  // modify_order() takes the cancel-then-reattach path — losing priority
  // (placing at tail of FIFO at the new price).
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(1, 1024, cb);

  assert(engine.add_order(1, 0, Side::Buy, 100ULL, 5, 1));
  assert(engine.add_order(2, 0, Side::Buy, 100ULL, 5, 2));
  // id=1 modifies to a different price — it should be re-anchored and
  // no longer at the head of any level.
  assert(engine.modify_order(1, 99ULL, 4));
  const Order* o = engine.find_order(1);
  assert(o != nullptr);
  assert(o->price == 99ULL);
  assert(o->qty_remaining == 4);
  assert(engine.level_qty(0, 99ULL) == 4);
  assert(engine.level_qty(0, 100ULL) == 5);
  std::printf("[ok] test_modified_order_loses_priority\n");
}

void test_execute_order_historical_fill() {
  // Historical execution: the resting order accepts an external execute.
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(1, 1024, cb);

  assert(engine.add_order(1, 0, Side::Buy, 100ULL, 10, 1));
  // 4-share fill from a taker — engine reports + decrements qty_remaining.
  Qty unfilled = engine.execute_order(1, 4, 2);
  assert(unfilled == 0);
  assert(cb.fill_count == 1);
  assert(cb.fills[0].order_id == 1);
  assert(cb.fills[0].qty == 4);
  assert(engine.find_order(1)->qty_remaining == 6);
  assert(engine.level_qty(0, 100ULL) == 6);
  std::printf("[ok] test_execute_order_historical_fill\n");
}

void test_market_order_sweep_and_ioc() {
  // Market order (IOC) sweeps the opposite side, then cancels any residual.
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(1, 1024, cb);

  assert(engine.add_order(1, 0, Side::Sell,  99ULL, 3, 1));
  assert(engine.add_order(2, 0, Side::Sell, 100ULL, 5, 2));
  assert(engine.add_order(3, 0, Side::Sell, 101ULL, 4, 3));

  // Buy 7 at market: takes all 3 @ 99, then 4 @ 100 (one extra share at
  // 100 stays). Residual 0 — IOC fully filled.
  bool ok = engine.add_market_order(10, 0, Side::Buy, 7, 4);
  assert(ok);
  // 2 trades -> 2 fills under the 1-on_fill-per-trade contract
  // (see matching_engine.ipp::match_against_level).
  assert(cb.fill_count == 2);
  assert(cb.fills[0].price == 99ULL && cb.fills[0].qty == 3);
  assert(cb.fills[1].price == 100ULL && cb.fills[1].qty == 4);
  // Resting levels after the sweep: 100 has 1 left, 101 unchanged.
  assert(engine.level_qty(0, 99ULL) == 0);
  assert(engine.level_qty(0, 100ULL) == 1);
  assert(engine.level_qty(0, 101ULL) == 4);
  assert(engine.best_bid(0) == 0);  // no bids (we were the aggressor)
  assert(engine.best_ask(0) == 100ULL);

  // Second market order with insufficient depth: IOC cancels residually.
  // cb.fill_count is only ever incremented; reset it here or the second
  // IOC's 2 fills fold onto the first IOC's 2.
  cb.fills.clear();
  cb.fill_count = 0;
  ok = engine.add_market_order(11, 0, Side::Buy, 1000, 5);
  assert(ok);
  assert(cb.fill_count == 2);
  assert(cb.fills[0].qty == 1 && cb.fills[0].price == 100ULL);
  assert(cb.fills[1].qty == 4 && cb.fills[1].price == 101ULL);
  assert(engine.find_order(11) == nullptr);
  // All asks gone → best_ask returns to kPriceInvalid sentinel.
  assert(engine.best_ask(0) == kPriceInvalid);
  assert(engine.books()[0].is_consistent());
  std::printf("[ok] test_market_order_sweep_and_ioc\n");
}

void test_multi_symbol_independence() {
  // Operations on one symbol must never affect another symbol's book or pool.
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(/*num_symbols=*/4, /*pool=*/2048, cb);

  // Heavy activity on symbol 0 (each order at a distinct price so none
  // cross — every slot stays checked out).
  for (std::uint32_t i = 0; i < 100; ++i) {
    assert(engine.add_order(i + 1, /*sym=*/0, Side::Buy,
                            1'000'000ULL + i, /*qty=*/1, i));
  }
  // Tier on symbol 1 — two orders at the same price cross.
  assert(engine.add_order(1000, /*sym=*/1, Side::Buy,  99ULL, 7, 1000));
  assert(engine.add_order(1001, /*sym=*/1, Side::Sell, 99ULL, 3, 1001));
  // Multi-symbol interaction check: symbol 1 crosses, symbol 0 unaffected.
  // 1 fill under the 1-on_fill-per-trade contract
  // (see matching_engine.ipp::match_against_level).
  assert(cb.fill_count == 1);
  assert(cb.fills[0].symbol == 1);
  assert(engine.best_bid(0) == 1'000'099ULL);   // top of sym0 = highest inserted
  assert(engine.level_qty(0, 2'000'000ULL) == 0);  // outside the inserted range

  assert(engine.find_order(1000) != nullptr);
  assert(engine.find_order(1000)->qty_remaining == 4);  // 7 - 3
  assert(engine.books()[0].is_consistent());
  assert(engine.books()[1].is_consistent());
  std::printf("[ok] test_multi_symbol_independence\n");
}

void test_pool_exhaustion_returns_false() {
  // With pool_size=4 and four non-crossing resting orders, every slot is
  // checked out. The fifth add_order must return false and leave the engine
  // unchanged (no partial mutations).
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(
      /*num_symbols=*/1, /*pool=*/4, cb);

  // Four distinct prices so the bids do not cross each other.
  assert(engine.add_order(1, 0, Side::Buy, 100ULL, 5, 1));
  assert(engine.add_order(2, 0, Side::Buy, 101ULL, 5, 2));
  assert(engine.add_order(3, 0, Side::Buy, 102ULL, 5, 3));
  assert(engine.add_order(4, 0, Side::Buy, 103ULL, 5, 4));

  // Pool is now full. The 5th order must be rejected.
  assert(!engine.add_order(5, 0, Side::Buy, 104ULL, 5, 5));
  assert(engine.find_order(5) == nullptr);

  // Cancelling a resting order returns its slot to the pool, after which
  // a new add_order succeeds.
  assert(engine.cancel_order(1));
  assert(engine.add_order(6, 0, Side::Buy, 104ULL, 5, 6));
  assert(engine.find_order(6) != nullptr);
  std::printf("[ok] test_pool_exhaustion_returns_false\n");
}

void test_top_of_book_invalidation() {
  // Sorted price vectors must be kept in sync after every insert/erase —
  // best_bid / best_ask come from the vectors' front, not from the hash map.
  CaptureCallback cb;
  MatchingEngine<CaptureCallback&> engine(1, 1024, cb);

  // Insert out-of-order prices; verify the cache says the same as the map.
  assert(engine.add_order(1, 0, Side::Buy, 105ULL, 1, 1));
  assert(engine.add_order(2, 0, Side::Buy, 110ULL, 1, 2));
  assert(engine.add_order(3, 0, Side::Buy, 100ULL, 1, 3));
  assert(engine.add_order(4, 0, Side::Buy, 108ULL, 1, 4));
  assert(engine.best_bid(0) == 110ULL);  // max bid

  // Remove the top bid — next-best takes over.
  assert(engine.cancel_order(2));
  assert(engine.best_bid(0) == 108ULL);
  assert(engine.cancel_order(4));
  assert(engine.best_bid(0) == 105ULL);
  assert(engine.cancel_order(1));
  assert(engine.cancel_order(3));
  assert(engine.best_bid(0) == 0);
  std::printf("[ok] test_top_of_book_invalidation\n");
}

}  // namespace

int main() {
  test_execution_report_layout();
  test_empty_book();
  test_add_no_match();
  test_price_time_priority();
  test_partial_fill();
  test_cancel_fully_removes();
  test_reduce_order_partial_cancel();
  test_modified_order_loses_priority();
  test_execute_order_historical_fill();
  test_market_order_sweep_and_ioc();
  test_multi_symbol_independence();
  test_pool_exhaustion_returns_false();
  test_top_of_book_invalidation();
  std::printf("\nAll %d engine tests passed.\n", 13);
  return 0;
}
