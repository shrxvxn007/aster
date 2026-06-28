// Aster — Backtest implementation.

#include "aster/strategy/backtest.hpp"
#include "aster/replay/replay_engine.hpp"

// Include the matching engine implementation so the template is instantiated
// with the complete Backtest type. This must come after backtest.hpp so that
// the Backtest class is fully defined.
#include "aster/core/matching_engine.ipp"

#include <cstdio>

namespace aster::strategy {

// Agent order IDs start above any historical ID range.
static constexpr OrderID kAgentIdStart = 100'000'000ULL;

Backtest::Backtest(BacktestConfig config)
    : config_(config),
      parser_(config.itch_file),
      engine_(config.num_symbols, config.pool_size, *this),
      analytics_(config.analytics),
      strategy_(config.strategy) {
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
            },
            msg);
      });

  replay_ = &replay;
  replay.run();
  replay_ = nullptr;

  analytics_.print();
}

void Backtest::on_fill(const ExecutionReport& r) {
  bool is_agent = is_agent_order_[r.order_id] != 0;
  analytics_.on_fill(r, !is_agent);  // agent is maker if its order was passive
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
  // If this is a symbol we haven't quoted on, start quoting.
  if (m.symbol < active_symbols_.size() && !active_symbols_[m.symbol]) {
    active_symbols_[m.symbol] = true;
    quote_symbol(m.symbol, recv_ts);
  }
}

void Backtest::handle_execute(const replay::OrderExecuteMsg& m,
                              Timestamp recv_ts) {
  // The engine has the resting order. engine_.execute_order emits fill reports
  // via on_fill and updates the book.
  (void)engine_.execute_order(m.order_id, m.qty, m.timestamp);

  // Update queue tracker for other agent orders at the same price.
  const Order* o = engine_.find_order(m.order_id);
  if (o) {
    tracker_.on_level_event(o->symbol, o->price, m.qty);
  }
}

void Backtest::handle_cancel(const replay::OrderCancelMsg& m,
                             Timestamp recv_ts) {
  const Order* o = engine_.find_order(m.order_id);
  if (o) {
    tracker_.on_level_event(o->symbol, o->price, m.qty);
  }
  (void)engine_.cancel_order(m.order_id);
}

void Backtest::handle_delete(const replay::OrderDeleteMsg& m,
                             Timestamp /*recv_ts*/) {
  (void)engine_.cancel_order(m.order_id);
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
  Price bb = engine_.best_bid(sym);
  Price ba = engine_.best_ask(sym);
  double mid = 0.0;
  if (bb > 0 && ba < kPriceInvalid) {
    mid = (to_price(bb) + to_price(ba)) * 0.5;
  } else if (bb > 0) {
    mid = to_price(bb) + config_.strategy.base_spread;
  } else if (ba < kPriceInvalid) {
    mid = to_price(ba) - config_.strategy.base_spread;
  } else {
    return;  // no market, can't quote
  }

  auto inv = strategy_.inventory(sym);
  auto q = strategy_.compute_quote(mid, inv, config_.strategy.T);
  if (q.bid != kPriceInvalid) {
    OrderID id = next_agent_id_++;
    (void)engine_.add_order(id, sym, Side::Buy, q.bid,
                            config_.strategy.base_lot, recv_ts);
    active_agent_orders_[sym].push_back(id);
  }
  if (q.ask != kPriceInvalid) {
    OrderID id = next_agent_id_++;
    (void)engine_.add_order(id, sym, Side::Sell, q.ask,
                            config_.strategy.base_lot, recv_ts);
    active_agent_orders_[sym].push_back(id);
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
