// Aster — ReplayEngine.
//
// Streams ITCH messages through a callback with configurable speed and latency
// injection. Deterministic: same file + same config → identical dispatch order.

#pragma once

#include "parser.hpp"
#include "profiler.hpp"

#include "aster/core/types.hpp"

#include <cstdint>
#include <functional>

namespace aster::replay {

// Configuration for the replay engine.
struct ReplayConfig {
  enum class SpeedMode { RealTime, Batch, Scaled };
  SpeedMode mode = SpeedMode::Batch;
  double speed_factor = 1.0;          // for Scaled mode
  // One-way latency injection: time for a message to travel from exchange to
  // the trader's system (applied to all inbound messages).
  std::uint64_t latency_exch_to_trader_ns = 0;
  // Trader-to-exchange latency: simulates the outbound round-trip. All
  // received messages are additionally delayed by this amount so that the
  // agent observes market data and receives fills/executions as if its own
  // orders had to travel to the exchange and back.
  std::uint64_t latency_trader_to_exch_ns = 0;
};

// Callback signature: receives (Message, recv_timestamp_ns).
using ReplayCallback =
    std::function<void(const Message& msg, std::uint64_t recv_timestamp_ns)>;

class ReplayEngine {
 public:
  ReplayEngine(ItchParser& parser, ReplayConfig config, ReplayCallback cb)
      : parser_(parser), config_(config), callback_(std::move(cb)) {}

  // Runs the full replay. Returns the number of messages dispatched.
  std::uint64_t run(Profiler* profiler = nullptr);

 private:
  ItchParser& parser_;
  ReplayConfig config_;
  ReplayCallback callback_;
};

}  // namespace aster::replay
