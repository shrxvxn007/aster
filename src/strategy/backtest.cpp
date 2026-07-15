// Aster — Backtest implementation.

#include "aster/strategy/backtest.hpp"
#include "aster/replay/replay_engine.hpp"

// Include the matching engine implementation so the template is instantiated
// with the complete Backtest type. This must come after backtest.hpp so that
// the Backtest class is fully defined.
#include "aster/core/matching_engine.ipp"

#include <algorithm>
#include <cstdio>
#include <functional>

namespace aster::strategy {

// Agent order IDs start above any historical ID range.
static constexpr OrderID kAgentIdStart = 100'000'000ULL;

Backtest::Backtest(BacktestConfig config)
    : config_(config),
      parser_(config.itch_file),
      engine_(config.num_symbols, config.pool_size, *this),
      analytics_(config.analytics),
      strategy_(config.strategy),
      risk_(config.risk) {
  active_symbols_.resize(config.num_symbols, false);
  active_agent_orders_.resize(config.num_symbols);
}

Backtest::~Backtest() {
  delete replay_;
}

void Backtest::run() {
  if (!parser_.is_open()) {
    std::fprintf(stderr, "Failed to open ITCH file: %s\n",
                 config_.itch_file.c_str());
    return;
  }

  ReplayEngine replay(
      parser_, config_.replay,
      [this](const replay::Message& msg, Timestamp recv_ts) {
        std::visit(
            [&](const auto& m) {
              using T = std::decay_t<decltype(m)>;
              if constexpr (std::is_same_v<T, replay::OrderAddMsg>)
                handle_add(m, recv_ts);
              else if constexpr (std::is_same_v<T, replay::OrderExecuteMsg>)
                handle_execute(m, recv_ts);
              else if constexpr (std::is_same_v<T, replay::OrderCancelMsg>)
                handle_cancel(m, recv_ts);
              else if constexpr (std::is_same_v<T, replay::OrderDeleteMsg>)
                handle_delete(m, recv_ts);
              else if constexpr (std::is_same_v<T, replay::SystemEventMsg>)
                handle_system(m, recv_ts);
              else if constexpr (std::is_same_v<T, replay::L2AggregateMsg>)
                handle_l2(m, recv_ts);
            },
            msg);
      });

  replay_ = &replay;

  // Profile the full replay callback path (parse + match + analytics) to get
  // nanosecond-level latency histograms for the end-to-end backtest.
  replay::Profiler profiler;
  replay.run(&profiler);
  replay_ = nullptr;

  analytics_.print();
  profiler.print("backtest");
}

void Backtest::on_fill(const ExecutionReport& r) {
  bool is_agent = is_agent_order_[r.order_id] != 0;
  analytics_.on_fill(r, !is_agent, r.timestamp);
  if (is_agent) {
    // Update strategy inventory.
    auto inv = strategy_.inventory(r.symbol);
    auto delta = (r.side == Side::Buy) ? static_cast<std::int64_t>(r.qty)
                                        : -static_cast<std::int64_t>(r.qty);
    strategy_.set_inventory(r.symbol, inv + delta);
    // Remove from queue tracker and per-symbol list.
    tracker_.remove(r.order_id);
    forget_agent_order(r.symbol, r.order_id);
    is_agent_order_.erase(r.order_id);
  }
  // Mark-to-market on every fill so adverse selection classification and
  // unrealized PnL reflect tape data in real time.
  update_mark_to_market(r.symbol);
}

void Backtest::on_accept(const Order& o, Timestamp ts) {
  // If this is an agent order, register it in the tracker.
  // Agent IDs start at kAgentIdStart; next_agent_id_ is already incremented
  // by the time on_accept is called (post-increment in add_order call).
  if (o.order_id >= kAgentIdStart) {
    is_agent_order_[o.order_id] = 1;
    // Compute volume ahead at this price.
    Qty ahead = engine_.level_qty(o.symbol, o.price);
    tracker_.on_agent_order(o.order_id, o.symbol, o.price, o.qty_remaining,
                            ahead);
  }
}

void Backtest::handle_add(const replay::OrderAddMsg& m, Timestamp recv_ts) {
  (void)engine_.add_order(m.order_id, m.symbol, m.side, m.price, m.qty,
                          m.timestamp);
  // Mark-to-market after every book change so analytics tracks unrealized PnL
  // and adverse selection on the current mid, not just on fills.
  update_mark_to_market(m.symbol);
  // If this is a symbol we haven't quoted on, start quoting.
  if (m.symbol < active_symbols_.size() && !active_symbols_[m.symbol]) {
    active_symbols_[m.symbol] = true;
    quote_symbol(m.symbol, recv_ts);
  }
}

void Backtest::handle_execute(const replay::OrderExecuteMsg& m,
                              Timestamp recv_ts) {
  // Validate the ITCH execute qty against the resting order's remaining.
  // An 'E' message qty exceeding the remaining is a data error — log and
  // clamp rather than silently dropping the excess.
  const Order* pre = engine_.find_order(m.order_id);
  Qty fill_qty = m.qty;
  if (pre && m.qty > pre->qty_remaining) {
    fill_qty = pre->qty_remaining;
  }

  // The engine has the resting order. engine_.execute_order emits fill reports
  // via on_fill and updates the book.
  (void)engine_.execute_order(m.order_id, fill_qty, m.timestamp);

  // Update queue tracker for other agent orders at the same price.
  const Order* o = engine_.find_order(m.order_id);
  if (o) {
    tracker_.on_level_event(o->symbol, o->price, fill_qty);
    update_mark_to_market(o->symbol);
    // Update market-order arrival rate for fill-probability estimation.
    update_arrival_rate(o->symbol, fill_qty, recv_ts);
  }
}

void Backtest::handle_cancel(const replay::OrderCancelMsg& m,
                             Timestamp recv_ts) {
  const Order* o = engine_.find_order(m.order_id);
  SymbolID sym = o ? o->symbol : 0;
  if (o) {
    tracker_.on_level_event(o->symbol, o->price, m.qty);
  }
  // ITCH 'C' supports partial cancels (qty < remaining). Use reduce_order
  // which handles both partial and full cancellation correctly.
  (void)engine_.reduce_order(m.order_id, m.qty);
  update_mark_to_market(sym);
}

void Backtest::handle_delete(const replay::OrderDeleteMsg& m,
                             Timestamp /*recv_ts*/) {
  const Order* o = engine_.find_order(m.order_id);
  SymbolID sym = o ? o->symbol : 0;
  (void)engine_.cancel_order(m.order_id);
  update_mark_to_market(sym);
}

void Backtest::handle_l2(const replay::L2AggregateMsg& m,
                         Timestamp /*recv_ts*/) {
  // Bounds guard: m.symbol must fit within the engine's pre-allocated
  // book array. L2 packets referencing an unknown symbol are treated as
  // data anomalies and ignored rather than crashing via out-of-bound
  // access into engine_.books(). Note that sibling handle_add does NOT
  // guard this explicitly — it relies on engine_.add_order's internal
  // symbol check. We dereference engine_.books()[m.symbol] directly, so
  // the unbounded vector operator[] warrants the explicit guard here.
  if (m.symbol >= engine_.books().size()) return;

  // Cross-side collision guard. OrderBook keyes `levels` by Price only
  // (see include/aster/core/order_book.hpp:7-9), so a Buy order and a
  // Sell order at the same price share one LevelQueue. If we blindly
  // override total_qty for one side, we'd corrupt the cached aggregate
  // for the *other* side at that price. We refuse to snap in either case:
  //   (a) the price is in both `bids` AND `asks` (locked/crossed market —
  //       rare on real tape; on those ticks the cache lags the truth).
  //   (b) the L2 msg's own `side` does not match a side that has this
  //       price in its sorted vector (data anomaly: a Sell-tagged message
  //       arriving against a bid-only level would overwrite a bid-side
  //       total with a sell-side number — actively wrong).
  // On reject we also skip the queue-tracker decrement: when both sides
  // collide OR the L2 side is unreliable, `old_total - m.qty` could
  // equally reflect vanished bid-side or ask-side shares, and we cannot
  // tell which. So the conservative answer is: no snap, no decrement.
  const auto& book = engine_.books()[m.symbol];
  const bool in_bids = std::binary_search(book.bids.begin(), book.bids.end(),
                                          m.price, std::greater<Price>{});
  const bool in_asks = std::binary_search(book.asks.begin(), book.asks.end(),
                                          m.price);
  const bool our_side_has =
      (m.side == Side::Buy) ? in_bids : in_asks;
  const bool other_side_has =
      (m.side == Side::Buy) ? in_asks : in_bids;
  if (!our_side_has || other_side_has) return;

  // Read the level's current cached total. level_qty() returns 0 when
  // the price is absent, so this works for pure-L2 streams (level empty
  // because no L3 OrderAdd has populated it) as well as mixed-mode feeds.
  Qty old_total = engine_.level_qty(m.symbol, m.price);
  if (m.qty < old_total) {
    // The exchange snapshot says fewer shares rest here than the engine's
    // cached view. Some of those vanished shares may have been *ahead of*
    // any agent orders sitting at this price level; decrement the queue
    // tracker's volume_ahead accordingly so the next quoted mid reflects
    // the actually observable ahead.
    tracker_.on_level_event(m.symbol, m.price, old_total - m.qty);
  }

  // Snap the level's cached total to the L2 snapshot in both directions.
  // For growth (m.qty > old_total) we *deliberately do NOT* grow any
  // agent's volume_ahead: L2 carries no order IDs so we cannot know
  // whether the fresh shares ended up ahead of or behind us, and picking
  // wrong would create an artificially inflated fill-probability. The
  // level's total_qty_ — used by engine_.level_qty() for fill-probability
  // queries — is updated so the strategy still sees the level's true depth.
  auto* lvl = engine_.books()[m.symbol].levels.find(m.price);
  if (lvl) {
    lvl->override_total_qty(m.qty);
  }

  // Update analytics' mark-to-market so unrealized PnL, drawdown, and the
  // adverse-selection classifier see the new top-of-book on every L2 tick.
  update_mark_to_market(m.symbol);
}

void Backtest::handle_system(const replay::SystemEventMsg& m,
                             Timestamp recv_ts) {
  // On market open, start quoting. On close, cancel all.
  switch (m.code) {
    case replay::SystemEventCode::MarketOpen:
      for (SymbolID s = 0; s < active_symbols_.size(); ++s) {
        if (active_symbols_[s]) quote_symbol(s, recv_ts);
      }
      break;
    case replay::SystemEventCode::MarketClose:
      for (SymbolID s = 0; s < active_symbols_.size(); ++s) {
        cancel_quotes(s, recv_ts);
      }
      break;
    default:
      break;
  }
}

void Backtest::quote_symbol(SymbolID sym, Timestamp recv_ts) {
  // Risk check: halt trading if drawdown kill-switch is triggered.
  if (!risk_.trading_allowed()) {
    // Cancel existing quotes and stop quoting.
    cancel_quotes(sym, recv_ts);
    return;
  }

  // Check drawdown kill-switch.
  if (!risk_.check_drawdown(analytics_.max_drawdown())) {
    cancel_quotes(sym, recv_ts);
    return;
  }

  Price bb = engine_.best_bid(sym);
  Price ba = engine_.best_ask(sym);
  double mid = 0.0;
  if (bb > 0 && ba < kPriceInvalid) {
    // Compute mid in fixed-point (Price is scaled 1e5) then convert. This
    // avoids double rounding that could differ across compilers with
    // -ffast-math or different optimization levels.
    mid = static_cast<double>((bb + ba) / 2) / static_cast<double>(kPriceScale);
  } else if (bb > 0) {
    mid = to_price(bb) + config_.strategy.base_spread;
  } else if (ba < kPriceInvalid) {
    mid = to_price(ba) - config_.strategy.base_spread;
  } else {
    return;  // no market, can't quote
  }

  auto inv = strategy_.inventory(sym);
  auto q = strategy_.compute_quote(mid, inv, config_.strategy.T);

  // React to adverse selection: if recent fills have been toxic (mid moved
  // against our position), widen the spread to compensate for the cost of
  // being adversely selected.
  double toxic_ratio = analytics_.toxic_fill_ratio();
  if (toxic_ratio > 0.0) {
    double mult = MmStrategy::toxicity_spread_multiplier(toxic_ratio);
    // Widen symmetrically around the mid.
    double bid_price = to_price(q.bid) - (mult - 1.0) * config_.strategy.base_spread;
    double ask_price = to_price(q.ask) + (mult - 1.0) * config_.strategy.base_spread;
    if (bid_price > 0.0) q.bid = from_double(bid_price);
    q.ask = from_double(ask_price);
  }

  // Adjust quotes using fill-probability estimates from the queue tracker
  // and the per-symbol market-order arrival rate.
  double lambda = (sym < rates_.size()) ? rates_[sym].lambda : 0.0;
  if (lambda > 0.0) {
    // Compute fill probability for the bid and ask using volume_ahead
    // from the queue tracker. If the order would be deep in queue,
    // fill probability is low and we shift the quote outward (widen).
    double horizon = config_.strategy.T;
    // For the bid side: look at volume ahead at the bid price.
    if (q.bid != kPriceInvalid) {
      Qty ahead_bid = engine_.level_qty(sym, q.bid);
      double p_fill_bid = MmStrategy::fill_probability(
          static_cast<double>(ahead_bid), lambda, horizon);
      // Widen the bid half-spread inversely with fill probability.
      // If p_fill ~ 1 (at front), no adjustment. If p_fill ~ 0 (deep),
      // shift the bid down by up to 2 extra ticks.
      if (p_fill_bid < 1.0) {
        double penalty = (1.0 - p_fill_bid) * 2.0 * config_.strategy.tick_size;
        double adj_bid = to_price(q.bid) - penalty;
        if (adj_bid > 0.0) q.bid = from_double(adj_bid);
      }
    }
    // Same for the ask side.
    if (q.ask != kPriceInvalid) {
      Qty ahead_ask = engine_.level_qty(sym, q.ask);
      double p_fill_ask = MmStrategy::fill_probability(
          static_cast<double>(ahead_ask), lambda, horizon);
      if (p_fill_ask < 1.0) {
        double penalty = (1.0 - p_fill_ask) * 2.0 * config_.strategy.tick_size;
        double adj_ask = to_price(q.ask) + penalty;
        q.ask = from_double(adj_ask);
      }
    }
  }

  // Place bid order if it passes risk checks.
  if (q.bid != kPriceInvalid) {
    auto inv = strategy_.inventory(sym);
    Qty lot = config_.strategy.base_lot;
    if (risk_.check_position_limit(sym, Side::Buy, lot, inv) &&
        risk_.record_order(sym, recv_ts)) {
      OrderID id = next_agent_id_++;
      (void)engine_.add_order(id, sym, Side::Buy, q.bid, lot, recv_ts);
      active_agent_orders_[sym].push_back(id);
    }
  }
  // Place ask order if it passes risk checks.
  if (q.ask != kPriceInvalid) {
    auto inv = strategy_.inventory(sym);
    Qty lot = config_.strategy.base_lot;
    if (risk_.check_position_limit(sym, Side::Sell, lot, inv) &&
        risk_.record_order(sym, recv_ts)) {
      OrderID id = next_agent_id_++;
      (void)engine_.add_order(id, sym, Side::Sell, q.ask, lot, recv_ts);
      active_agent_orders_[sym].push_back(id);
    }
  }
}

void Backtest::cancel_quotes(SymbolID sym, Timestamp /*recv_ts*/) {
  if (sym >= active_agent_orders_.size()) return;
  auto& ids = active_agent_orders_[sym];
  for (OrderID id : ids) {
    tracker_.remove(id);
    is_agent_order_.erase(id);
    (void)engine_.cancel_order(id);
  }
  ids.clear();
}

void Backtest::update_arrival_rate(SymbolID sym, Qty qty, Timestamp ts) {
  if (sym >= rates_.size()) rates_.resize(static_cast<std::size_t>(sym) + 1);
  auto& r = rates_[sym];
  if (!r.initialized) {
    // First observation: seed the EMA with a rough instantaneous rate.
    r.lambda = static_cast<double>(qty);  // shares per "unit" — refined over time
    r.last_ts = ts;
    r.initialized = true;
    return;
  }
  double dt = static_cast<double>(ts - r.last_ts) * 1e-9;  // ns → seconds
  if (dt <= 0.0) return;
  double inst_rate = static_cast<double>(qty) / dt;  // shares/second
  r.lambda += kLambdaAlpha * (inst_rate - r.lambda);
  r.last_ts = ts;
}

void Backtest::update_mark_to_market(SymbolID sym) {
  if (sym >= engine_.books().size()) return;
  Price bb = engine_.best_bid(sym);
  Price ba = engine_.best_ask(sym);
  double mid = 0.0;
  if (bb > 0 && ba < kPriceInvalid) {
    // Fixed-point mid to avoid cross-build double rounding differences.
    mid = static_cast<double>((bb + ba) / 2) / static_cast<double>(kPriceScale);
  } else if (bb > 0) {
    mid = to_price(bb);
  } else if (ba < kPriceInvalid) {
    mid = to_price(ba);
  } else {
    return;  // no market, can't mark
  }
  analytics_.mark_to_market(sym, mid);
}

void Backtest::forget_agent_order(SymbolID sym, OrderID id) {
  if (sym >= active_agent_orders_.size()) return;
  auto& ids = active_agent_orders_[sym];
  for (auto it = ids.begin(); it != ids.end(); ++it) {
    if (*it == id) {
      ids.erase(it);
      return;
    }
  }
}

}  // namespace aster::strategy
