// Aster — standalone synthetic matching engine benchmark.
//
// Generates a Poisson order-arrival process and loops it through the engine,
// printing profiler stats. No ITCH file required.
//
// Usage: aster_sim --pool <size> --events <count> [--symbols <n>]

#include "aster/core/matching_engine.hpp"
#include "aster/core/types.hpp"
#include "aster/replay/profiler.hpp"
#include "aster/utils/timestamp.h"

// Explicitly include the matching engine implementation for the
// MatchingEngine<NullCallback&> instantiation.
#include "aster/core/matching_engine.ipp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace aster;
using namespace aster::replay;

// A simple callback that does nothing (just counts).
struct NullCallback {
  std::uint64_t fill_count = 0;
  std::uint64_t accept_count = 0;
  void on_fill(const ExecutionReport& r) { ++fill_count; }
  void on_accept(const Order&, Timestamp) { ++accept_count; }
};

int main(int argc, char** argv) {
  std::uint32_t pool_size = 100'000;
  std::uint64_t events = 1'000'000;
  std::uint32_t num_symbols = 16;
  std::uint64_t seed = 42;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--pool") == 0 && i + 1 < argc) {
      pool_size = static_cast<std::uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--events") == 0 && i + 1 < argc) {
      events = static_cast<std::uint64_t>(std::atoll(argv[++i]));
    } else if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) {
      num_symbols = static_cast<std::uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = static_cast<std::uint64_t>(std::atoll(argv[++i]));
    } else if (std::strcmp(argv[i], "--help") == 0) {
      std::printf("Usage: aster_sim --pool <size> --events <count> "
                  "[--symbols <n>] [--seed <n>]\n");
      return 0;
    }
  }

  NullCallback cb;
  MatchingEngine<NullCallback&> engine(num_symbols, pool_size, cb);

  // Deterministic PRNG (xoshiro256**).
  auto splitmix64 = [](std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  };

  std::uint64_t rng = seed;
  Price base_price = from_double(100.0);  // 100.00000
  Price tick = from_double(0.01);         // 0.01000

  Profiler profiler;
  auto start = now_ns();

  for (std::uint64_t i = 0; i < events; ++i) {
    Side side = (splitmix64(rng) & 1) ? Side::Buy : Side::Sell;
    SymbolID sym = static_cast<SymbolID>(splitmix64(rng) % num_symbols);
    // Price around base: ±100 ticks.
    std::int64_t offset = static_cast<std::int64_t>(splitmix64(rng) % 201) - 100;
    Price price = base_price + static_cast<Price>(offset) * tick;
    Qty qty = static_cast<Qty>(splitmix64(rng) % 10) + 1;
    OrderID id = i + 1;
    Timestamp ts = static_cast<Timestamp>(i * 1000);  // 1us spacing

    profiler.measure([&] {
      (void)engine.add_order(id, sym, side, price, qty, ts);
    });
  }

  auto elapsed = now_ns() - start;

  std::printf("=== Synthetic Benchmark ===\n");
  std::printf("Events:    %llu\n", static_cast<unsigned long long>(events));
  std::printf("Fills:     %llu\n", static_cast<unsigned long long>(cb.fill_count));
  std::printf("Accepts:   %llu\n", static_cast<unsigned long long>(cb.accept_count));
  std::printf("Elapsed:   %.3f ms\n", static_cast<double>(elapsed) / 1e6);
  std::printf("Throughput: %.2f M events/sec\n",
              static_cast<double>(events) / (static_cast<double>(elapsed) / 1e9) / 1e6);
  profiler.print("engine");

  return 0;
}
