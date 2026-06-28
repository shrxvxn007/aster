// Aster — Backtest composition root.
//
// Wires the matching engine, replay engine, MM strategy, queue tracker, and
// analytics together. The Backtest also acts as the engine's callback (it
// inherits from a passive callback interface).

#pragma once

#include "aster/core/matching_engine.hpp"
#include "aster/core/types.hpp"
#include "aster/replay/parser.hpp"
#include "aster/replay/replay_engine.hpp"

#include "aster/strategy/analytics.hpp"
#include "aster/strategy/mm_strategy.hpp"
#include "aster/strategy/queue_tracker.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aster::strategy {

using replay::ItchParser;
using replay::ReplayConfig;
using replay::ReplayEngine;

// Backtest configuration.
struct BacktestConfig {
  std::filesystem::path itch_file;
  std::uint32_t num_symbols = 256;
  std::uint32_t pool_size = 1'000'000;
  ReplayConfig replay;
  MmParams strategy;
  AnalyticsConfig analytics;
};

// The Backtest acts as the Callback type for the MatchingEngine.
// It must provide on_fill(const ExecutionReport&) and on_accept(const Order&, Timestamp).
class Backtest {
 public:
  explicit Backtest(BacktestConfig config);
  ~Backtest();

  // Run the full backtest.
  void run();

  // Post-run access.
  const Analytics& analytics() const { return analytics_; }

  // Callback interface for the matching engine.
  void on_fill(const ExecutionReport& r);
  void on_accept(const Order& o, Timestamp ts);

  // Accessors.
  MatchingEngine<Backtest&>& engine() { return engine_; }
  QueueTracker& tracker() { return tracker_; }

 private:
  BacktestConfig config_;
  ItchParser parser_;
  MatchingEngine<Backtest&> engine_;
  QueueTracker tracker_;
  Analytics analytics_;
  MmStrategy strategy_;
  ReplayEngine* replay_ = nullptr;

  // Agent order ID management.
  OrderID next_agent_id_ = 100'000'000ULL;  // kAgentIdStart, above any historical ID

  // Track which symbols we've quoted on.
  std::vector<bool> active_symbols_;
  // Historical order -> was it from the agent? (can't use bool because
  // vector<bool> specialization is incompatible with flat_hash_map)
  flat_hash_map<OrderID, std::uint8_t> is_agent_order_;
  // Per-symbol list of active agent order IDs. Enables O(k) cancellation on
  // market-close where k = agent orders on that symbol (typically 2).
  std::vector<std::vector<OrderID>> active_agent_orders_;

  // Handle different message types.
  void handle_add(const replay::OrderAddMsg& m, Timestamp recv_ts);
  void handle_execute(const replay::OrderExecuteMsg& m, Timestamp recv_ts);
  void handle_cancel(const replay::OrderCancelMsg& m, Timestamp recv_ts);
  void handle_delete(const replay::OrderDeleteMsg& m, Timestamp recv_ts);
  void handle_system(const replay::SystemEventMsg& m, Timestamp recv_ts);

  // Quote management.
  void quote_symbol(SymbolID sym, Timestamp recv_ts);
  void cancel_quotes(SymbolID sym, Timestamp recv_ts);
  // Remove a single agent order from the per-symbol tracking (used on fill).
  void forget_agent_order(SymbolID sym, OrderID id);
};

}  // namespace aster::strategy
