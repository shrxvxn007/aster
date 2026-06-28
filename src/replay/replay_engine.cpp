// Aster — ReplayEngine implementation.

#include "aster/replay/replay_engine.hpp"

#include "aster/utils/timestamp.h"

#include <algorithm>
#include <cassert>
#include <thread>

#if defined(__APPLE__)
#  include <time.h>
#else
#  include <time.h>
#endif

namespace aster::replay {

std::uint64_t ReplayEngine::run(Profiler* profiler) {
  std::uint64_t dispatched = 0;
  Message msg;

  // For RealTime / Scaled mode, we anchor to the first event's timestamp.
  std::uint64_t first_ts = 0;
  std::uint64_t wall_start = 0;
  bool has_first = false;

  while (parser_.next(msg)) {
    // Extract the exchange timestamp from the message.
    std::uint64_t exch_ts = 0;
    std::visit([&](const auto& m) { exch_ts = m.timestamp; }, msg);

    // recv_timestamp = exchange timestamp + one-way latency.
    std::uint64_t recv_ts = exch_ts + config_.latency_exch_to_trader_ns;

    if (!has_first) {
      first_ts = exch_ts;
      wall_start = now_ns();
      has_first = true;
    }

    // Delay logic for RealTime / Scaled mode.
    if (config_.mode == ReplayConfig::SpeedMode::RealTime ||
        config_.mode == ReplayConfig::SpeedMode::Scaled) {
      std::uint64_t elapsed_exch = exch_ts - first_ts;
      std::uint64_t target_wall = wall_start + static_cast<std::uint64_t>(
          static_cast<double>(elapsed_exch) / config_.speed_factor);
      std::uint64_t now = now_ns();
      if (target_wall > now) {
        auto delay = target_wall - now;
        // Use nanosleep for sub-millisecond precision on macOS.
        struct timespec ts;
        ts.tv_sec = static_cast<time_t>(delay / 1'000'000'000ULL);
        ts.tv_nsec = static_cast<long>(delay % 1'000'000'000ULL);
        while (nanosleep(&ts, &ts) != 0) {
          // Retry on EINTR.
        }
      }
    }

    if (profiler) {
      profiler->measure([&] { callback_(msg, recv_ts); });
    } else {
      callback_(msg, recv_ts);
    }
    ++dispatched;
  }

  return dispatched;
}

}  // namespace aster::replay
