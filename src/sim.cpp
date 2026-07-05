// Aster — synthetic matching-engine benchmark with optional per-event
// latency injection.
//
// Single-run mode (used by CI's perf-floor job):
//   PRNG-driven Poisson order arrivals over a deterministic seed. Each
//   event is preceded by an optional busy-wait (--latency-exch) that
//   simulates the configured one-way exchange → trader latency. The
//   Profiler measures ONLY engine hot-path latency; total wall-clock
//   feeds Elapsed/Throughput, so a user can decompose "engine work"
//   vs "network wait" by inspecting the two columns side by side.
//
// Sweep mode (--sweep):
//   Runs the same engine at four latency levels (0 / 100 / 1k / 10k ns)
//   and emits a comparison table in a single invocation, so a reader can
//   see for themselves how throughput and tail-latency degrade as the
//   per-event latency budget grows. --sweep overrides --latency-exch.
//
// Allocation tracking: a global operator new/delete override in this TU
// catches any heap traffic during the per-event loop. The delta over
// the run is printed so a non-zero value flags a regression on the
// "zero-alloc hot path" contract.
//
// Usage examples:
//   ./build/aster_sim --events 1000000
//   ./build/aster_sim --events 1000000 --latency-exch 100
//   ./build/aster_sim --events 1000000 --sweep

#include "aster/core/matching_engine.hpp"
#include "aster/core/matching_engine.ipp"
#include "aster/core/types.hpp"
#include "aster/replay/profiler.hpp"
#include "aster/utils/timestamp.h"

#if defined(__APPLE__)
#  include <mach/mach.h>
#  include <sys/resource.h>
#else
#  include <sys/resource.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using namespace aster;
using namespace aster::replay;

// ---------------------------------------------------------------------------
// Global allocation counter.
//
// Overriding ::operator new/delete in this TU catches any heap traffic
// routed through std::allocator (which all STL containers use, including
// flat_hash_map's std::vector-of-pairs and OrderPool's std::vector<Order>).
// libc allocations (e.g. internally buffered stdio) are NOT covered by
// this counter; if we ever need them, an external LD_PRELOAD shim would
// do it without touching source.
// ---------------------------------------------------------------------------
namespace {
std::atomic<std::uint64_t> g_alloc_bytes{0};
std::atomic<std::uint64_t> g_alloc_count{0};
}  // namespace

void* operator new(std::size_t n) {
  g_alloc_bytes.fetch_add(n, std::memory_order_relaxed);
  g_alloc_count.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n);
}
void* operator new[](std::size_t n) {
  g_alloc_bytes.fetch_add(n, std::memory_order_relaxed);
  g_alloc_count.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  g_alloc_bytes.fetch_add(n, std::memory_order_relaxed);
  g_alloc_count.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(n);
}
void operator delete(void* p) noexcept {
  if (p) std::free(p);
}
void operator delete[](void* p) noexcept {
  if (p) std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
  if (p) std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  if (p) std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  if (p) std::free(p);
}

// ---------------------------------------------------------------------------
// Busy-wait helper.
//
// For sub-microsecond latencies nanosleep(2) is unreliable: the kernel
// rounds up to the OS scheduler tick (≥1µs typically) and adds jitter.
// A chrono spin with the same clock we use elsewhere is the only way
// to inject a known-magnitude delay under 1µs.
// ---------------------------------------------------------------------------
static inline void busy_wait_ns(std::uint64_t ns) noexcept {
  if (ns == 0) return;
  using clk = std::chrono::steady_clock;
  auto target = clk::now() + std::chrono::nanoseconds(ns);
  while (clk::now() < target) {
    // tight spin — steady_clock::now() is the rate limiter.
  }
}

// ---------------------------------------------------------------------------
// Resident-set size (KB). Linux glibc reports ru_maxrss in KB; macOS
// reports resident_size in bytes. We normalise to KB so the column is
// directly comparable across platforms.
// ---------------------------------------------------------------------------
static std::uint64_t rss_kb() noexcept {
#if defined(__APPLE__)
  task_basic_info info{};
  mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return 0;
  }
  return static_cast<std::uint64_t>(info.resident_size) / 1024ULL;
#else
  struct rusage ru{};
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
  // glibc reports ru_maxrss in KB (man getrusage, Linux). musl-based
  // distros (Alpine, embedded) report BYTES; divide for portability.
#  if defined(__GLIBC__)
  return static_cast<std::uint64_t>(ru.ru_maxrss);
#  else
  return static_cast<std::uint64_t>(ru.ru_maxrss) / 1024ULL;
#  endif
#endif
}

// ---------------------------------------------------------------------------
// Synthetic-bench callback.
// ---------------------------------------------------------------------------
struct SimCallback {
  std::uint64_t fill_count = 0;
  std::uint64_t accept_count = 0;
  void on_fill(const ExecutionReport&) { ++fill_count; }
  void on_accept(const Order&, Timestamp) { ++accept_count; }
};

// ---------------------------------------------------------------------------
// One bench iteration at a fixed per-event latency. Engine is reconstructed
// per run so startup allocations don't leak into per-event deltas; the
// seed is fixed so the deterministic random walk is identical run to run.
// ---------------------------------------------------------------------------
static void run_one(std::uint32_t pool_size, std::uint64_t events,
                    std::uint32_t num_symbols, std::uint64_t seed,
                    std::uint64_t latency_exch_ns,
                    std::uint64_t& out_fills, std::uint64_t& out_accepts,
                    std::uint64_t& out_alloc_bytes,
                    std::uint64_t& out_alloc_count, double& out_elapsed_ms,
                    double& out_throughput, Profiler& out_profiler,
                    std::uint64_t& out_rss_kb) {
  SimCallback cb;
  MatchingEngine<SimCallback&> engine(num_symbols, pool_size, cb);

  // Deterministic PRNG (splitmix64).
  std::uint64_t rng = seed;
  auto splitmix64 = [&rng]() {
    std::uint64_t z = (rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  };

  Price base_price = from_double(100.0);  // $100.00
  Price tick = from_double(0.01);         // $0.01

  // Snapshot alloc counters AFTER engine construction so the per-event
  // delta reflects only loop-body traffic. Any non-zero delta is a
  // regression on the zero-alloc hot-path contract.
  auto bytes_before = g_alloc_bytes.load();
  auto count_before = g_alloc_count.load();

  Profiler profiler;
  auto start = now_ns();
  for (std::uint64_t i = 0; i < events; ++i) {
    Side side = (splitmix64() & 1) ? Side::Buy : Side::Sell;
    SymbolID sym = static_cast<SymbolID>(splitmix64() % num_symbols);
    std::int64_t offset = static_cast<std::int64_t>(splitmix64() % 201) - 100;
    Price price = base_price + static_cast<Price>(offset) * tick;
    Qty qty = static_cast<Qty>(splitmix64() % 10) + 1;
    OrderID id = i + 1;
    Timestamp ts = static_cast<Timestamp>(i * 1000);

    // Inject the simulated network delay OUTSIDE the per-event
    // measurement so the Profiler's histogram is engine-only.
    if (latency_exch_ns) busy_wait_ns(latency_exch_ns);

    profiler.measure([&] {
      (void)engine.add_order(id, sym, side, price, qty, ts);
    });
  }
  auto elapsed = now_ns() - start;

  out_fills = cb.fill_count;
  out_accepts = cb.accept_count;
  out_alloc_bytes = g_alloc_bytes.load() - bytes_before;
  out_alloc_count = g_alloc_count.load() - count_before;
  out_elapsed_ms = static_cast<double>(elapsed) / 1e6;
  out_throughput =
      static_cast<double>(events) / (static_cast<double>(elapsed) / 1e9) /
      1e6;
  out_profiler = std::move(profiler);
  out_rss_kb = rss_kb();
}

// ---------------------------------------------------------------------------
// CLI.
// ---------------------------------------------------------------------------
static void print_usage() {
  std::printf(
      "Usage: aster_sim --pool <N> --events <N> [options]\n"
      "\n"
      "Synthesises a deterministic Poisson order-arrival stream and\n"
      "loops it through the matching engine. Optional per-event latency\n"
      "(--latency-exch) and a 4-point sweep (--sweep) make this binary\n"
      "useful for characterising throughput and tail-latency degradation\n"
      "under stress and surfacing any unexpected heap traffic.\n"
      "\n"
      "Options:\n"
      "  --pool <N>              OrderPool capacity (default 100000).\n"
      "  --events <N>            Number of events to dispatch (default 1000000).\n"
      "  --symbols <N>           Number of symbols the events span "
      "(default 16).\n"
      "  --seed <N>              PRNG seed, fixed so runs are comparable "
      "(default 42).\n"
      "  --latency-exch <ns>     Inject a one-way exch→trader delay\n"
      "                          (busy-wait) before each event. 0 disables.\n"
      "                          Ignored if --sweep is also set.\n"
      "  --latency-trader <ns>   Trader→exch round-trip delay (informational\n"
      "                          only for the synthetic bench).\n"
      "  --sweep                 Run at latency-exch = 0/100/1000/10000 ns\n"
      "                          and print a columnar comparison table.\n"
      "                          Overrides --latency-exch.\n"
      "  --help, -h              Print this message.\n");
}

int main(int argc, char** argv) {
  std::uint32_t pool_size = 100'000;
  std::uint64_t events = 1'000'000;
  std::uint32_t num_symbols = 16;
  std::uint64_t seed = 42;
  bool sweep = false;
  std::uint64_t latency_exch_ns = 0;
  std::uint64_t latency_trader_ns = 0;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--pool") == 0 && i + 1 < argc) {
      pool_size = static_cast<std::uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--events") == 0 && i + 1 < argc) {
      events = static_cast<std::uint64_t>(std::atoll(argv[++i]));
    } else if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) {
      num_symbols = static_cast<std::uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = static_cast<std::uint64_t>(std::atoll(argv[++i]));
    } else if (std::strcmp(argv[i], "--latency-exch") == 0 && i + 1 < argc) {
      latency_exch_ns = static_cast<std::uint64_t>(std::atoll(argv[++i]));
    } else if (std::strcmp(argv[i], "--latency-trader") == 0 && i + 1 < argc) {
      latency_trader_ns = static_cast<std::uint64_t>(std::atoll(argv[++i]));
    } else if (std::strcmp(argv[i], "--sweep") == 0) {
      sweep = true;
    } else if (std::strcmp(argv[i], "--help") == 0 ||
               std::strcmp(argv[i], "-h") == 0) {
      print_usage();
      return 0;
    } else {
      std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      print_usage();
      return 1;
    }
  }

  if (events == 0) {
    std::fprintf(stderr, "Error: --events must be > 0\n");
    return 2;
  }

  if (sweep) {
    constexpr std::uint64_t kLatenciesNs[] = {0, 100, 1000, 10000};

    std::printf(
        "=== Latency Sweep (events=%llu, symbols=%u, seed=%llu) ===\n",
        static_cast<unsigned long long>(events),
        static_cast<unsigned>(num_symbols),
        static_cast<unsigned long long>(seed));

    // Column widths chosen so the row lines up in monospace terminals.
    std::printf(
        "%-10s %-12s %-12s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n",
        "exch_ns", "elapsed_ms", "thru(M/s)", "p50(ns)", "p99(ns)", "p99.9",
        "p99.99", "max(ns)", "alloc_B", "rss_KB");
    std::printf(
        "---------- ------------ ------------ ---------- ---------- "
        "---------- ---------- ---------- ---------- ----------\n");

    for (std::uint64_t lat : kLatenciesNs) {
      std::uint64_t fills = 0, accepts = 0, alloc_b = 0, alloc_n = 0, rss = 0;
      double elapsed_ms = 0.0, throughput = 0.0;
      Profiler prof;
      run_one(pool_size, events, num_symbols, seed, lat,
              fills, accepts, alloc_b, alloc_n,
              elapsed_ms, throughput, prof, rss);
      std::printf(
          "%-10llu %-12.3f %-12.2f %-10llu %-10llu %-10llu %-10llu "
          "%-10llu %-10llu %-10llu\n",
          static_cast<unsigned long long>(lat),
          elapsed_ms,
          throughput,
          static_cast<unsigned long long>(prof.percentile(0.5)),
          static_cast<unsigned long long>(prof.percentile(0.99)),
          static_cast<unsigned long long>(prof.percentile(0.999)),
          static_cast<unsigned long long>(prof.percentile(0.9999)),
          static_cast<unsigned long long>(prof.max_ns()),
          static_cast<unsigned long long>(alloc_b),
          static_cast<unsigned long long>(rss));
    }
    return 0;
  }

  // Single-run mode. Output format is unchanged from before the
  // latency-sweep work so the CI perf-floor awk pattern that scrapes
  // "Throughput:" lines continues to parse.
  std::uint64_t fills = 0, accepts = 0, alloc_b = 0, alloc_n = 0, rss = 0;
  double elapsed_ms = 0.0, throughput = 0.0;
  Profiler prof;
  run_one(pool_size, events, num_symbols, seed, latency_exch_ns,
          fills, accepts, alloc_b, alloc_n,
          elapsed_ms, throughput, prof, rss);

  std::printf("=== Synthetic Benchmark ===\n");
  std::printf("Events:    %llu\n", static_cast<unsigned long long>(events));
  std::printf("Fills:     %llu\n", static_cast<unsigned long long>(fills));
  std::printf("Accepts:   %llu\n", static_cast<unsigned long long>(accepts));
  std::printf("Elapsed:   %.3f ms\n", elapsed_ms);
  std::printf("Throughput: %.2f M events/sec\n", throughput);
  if (latency_exch_ns) {
    std::printf("Injected exch latency: %llu ns/event\n",
                static_cast<unsigned long long>(latency_exch_ns));
  }
  prof.print("engine");
  std::printf("Hot-path allocs: %llu bytes / %llu calls (loop delta)\n",
              static_cast<unsigned long long>(alloc_b),
              static_cast<unsigned long long>(alloc_n));
  return 0;
}
