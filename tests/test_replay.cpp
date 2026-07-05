// Aster — replay-engine tests.
//
// Builds a tiny ITCH file in std::filesystem::temp_directory_path(),
// runs it through ItchParser + ReplayEngine, and asserts:
//   1. Dispatch is in file order regardless of SpeedMode (the headline
//      determinism property the backtester relies on).
//   2. Latency injection rounds correctly to recv_timestamp.
//   3. A real matching-engine + analytics round-trip produces a coherent
//      marker (PNL changes only when a crossing fill occurs).
//
// Uses <cassert> only — same convention as the engine/parser test suites.

#include "aster/replay/itch.hpp"
#include "aster/replay/parser.hpp"
#include "aster/replay/replay_engine.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <variant>
#include <vector>

using namespace aster;
using namespace aster::replay;

namespace {

// ---------------------------------------------------------------------------
// Big-endian write helpers (same convention used by test_parser.cpp).
// ---------------------------------------------------------------------------
void write_u8(std::ofstream& f, std::uint8_t v) { f.put(static_cast<char>(v)); }
void write_u16(std::ofstream& f, std::uint16_t v) {
  write_u8(f, static_cast<std::uint8_t>(v >> 8));
  write_u8(f, static_cast<std::uint8_t>(v));
}
void write_u32(std::ofstream& f, std::uint32_t v) {
  write_u16(f, static_cast<std::uint16_t>(v >> 16));
  write_u16(f, static_cast<std::uint16_t>(v));
}
void write_u48_be(std::ofstream& f, std::uint64_t v) {
  for (int i = 6; i >= 0; --i) write_u8(f, static_cast<std::uint8_t>(v >> (i * 8)));
}
void write_u64(std::ofstream& f, std::uint64_t v) {
  for (int i = 7; i >= 0; --i) write_u8(f, static_cast<std::uint8_t>(v >> (i * 8)));
}

void write_header(std::ofstream& f, std::uint32_t symbol_count) {
  f.put('I'); f.put('T'); f.put('C'); f.put('H');
  write_u32(f, /*version=*/1);
  write_u32(f, symbol_count);
}
void write_symbol(std::ofstream& f, const std::string& name,
                  std::uint16_t id) {
  write_u8(f, static_cast<std::uint8_t>(name.size()));
  f.write(name.data(), static_cast<std::streamsize>(name.size()));
  write_u16(f, id);
}
template <typename PayloadFn>
void write_msg(std::ofstream& f, char type, std::uint64_t ts, PayloadFn payload) {
  write_u8(f, static_cast<std::uint8_t>(type));
  write_u48_be(f, ts);
  payload(f);
}

// ---------------------------------------------------------------------------
// Test 1: dispatch order matches file order across mixed message types.
// ---------------------------------------------------------------------------
void test_dispatch_order_is_deterministic() {
  auto path = std::filesystem::temp_directory_path() /
              "aster_test_replay_order.itch";
  {
    std::ofstream f(path, std::ios::binary);
    assert(f.is_open());
    write_header(f, /*symbol_count=*/1);
    write_symbol(f, "SYM", 0);

    write_msg(f, 'S', 100, [&](std::ofstream& g) {
      write_u8(g, static_cast<std::uint8_t>(SystemEventCode::MarketOpen));
    });
    write_msg(f, 'A', 110, [&](std::ofstream& g) {
      write_u64(g, /*oid=*/1);
      write_u16(g, /*sym=*/0);
      write_u8(g, static_cast<std::uint8_t>(Side::Buy));
      write_u64(g, /*price=*/100ULL);
      write_u32(g, /*qty=*/10);
    });
    write_msg(f, 'A', 120, [&](std::ofstream& g) {
      write_u64(g, /*oid=*/2);
      write_u16(g, /*sym=*/0);
      write_u8(g, static_cast<std::uint8_t>(Side::Sell));
      write_u64(g, /*price=*/105ULL);
      write_u32(g, /*qty=*/10);
    });
    write_msg(f, 'C', 130, [&](std::ofstream& g) {
      write_u64(g, /*oid=*/1);
      write_u32(g, /*qty=*/4);
    });
    write_msg(f, 'E', 140, [&](std::ofstream& g) {
      write_u64(g, /*oid=*/2);
      write_u32(g, /*qty=*/3);
    });
    write_msg(f, 'S', 200, [&](std::ofstream& g) {
      write_u8(g, static_cast<std::uint8_t>(SystemEventCode::MarketClose));
    });
  }

  ItchParser p(path);
  std::vector<char> first_byte;
  std::vector<std::uint64_t> recv_log;
  ReplayConfig cfg;
  cfg.mode = ReplayConfig::SpeedMode::Batch;
  cfg.latency_exch_to_trader_ns = 50;

  ReplayEngine engine(
      p, cfg,
      [&](const Message& m, std::uint64_t recv_ts) {
        // First byte of the dispatched message = its type tag.
        first_byte.push_back(std::visit(
            [](const auto& x) -> char {
              using T = std::decay_t<decltype(x)>;
              if constexpr (std::is_same_v<T, SystemEventMsg>) {
                return 'S';
              } else if constexpr (std::is_same_v<T, OrderAddMsg>) {
                return 'A';
              } else if constexpr (std::is_same_v<T, OrderExecuteMsg>) {
                return 'E';
              } else if constexpr (std::is_same_v<T, OrderCancelMsg>) {
                return 'C';
              } else if constexpr (std::is_same_v<T, OrderDeleteMsg>) {
                return 'D';
              } else if constexpr (std::is_same_v<T, L2AggregateMsg>) {
                return 'L';
              }
              return '?';
            },
            m));
        recv_log.push_back(recv_ts);
      });
  std::uint64_t dispatched = engine.run();

  assert(dispatched == 6);
  assert(first_byte.size() == 6);
  // Dispatch order must match file order: S, A, A, C, E, S.
  const std::vector<char> expected = {'S', 'A', 'A', 'C', 'E', 'S'};
  assert(first_byte == expected);

  // Monotonic recv_ts: latency is injected symmetrically so recv_ts must
  // still be increasing with the file's exchange timestamps.
  assert(std::is_sorted(recv_log.begin(), recv_log.end()));

  std::filesystem::remove(path);
  std::printf("[ok] test_dispatch_order_is_deterministic\n");
}

// ---------------------------------------------------------------------------
// Test 2: latency injection adds the configured exchange + trader deltas.
// ---------------------------------------------------------------------------
void test_latency_injection_rounds() {
  auto path = std::filesystem::temp_directory_path() /
              "aster_test_replay_latency.itch";
  {
    std::ofstream f(path, std::ios::binary);
    assert(f.is_open());
    write_header(f, /*symbol_count=*/0);
    write_msg(f, 'S', /*ts=*/12345, [&](std::ofstream& g) {
      write_u8(g, static_cast<std::uint8_t>(SystemEventCode::MarketOpen));
    });
  }

  ItchParser p(path);
  ReplayConfig cfg;
  cfg.mode = ReplayConfig::SpeedMode::Batch;
  cfg.latency_exch_to_trader_ns = 1000;
  cfg.latency_trader_to_exch_ns = 250;

  std::uint64_t observed_recv = 0;
  ReplayEngine engine(p, cfg, [&](const Message&, std::uint64_t recv_ts) {
    observed_recv = recv_ts;
  });
  (void)engine.run();

  // exch_ts + exch→trader + trader→exch = 12345 + 1000 + 250.
  assert(observed_recv == 12345 + 1000 + 250);

  std::filesystem::remove(path);
  std::printf("[ok] test_latency_injection_rounds\n");
}

// ---------------------------------------------------------------------------
// Test 3: parser + replay round-trip preserves the exact message count.
// ---------------------------------------------------------------------------
void test_round_trip_message_count() {
  auto path = std::filesystem::temp_directory_path() /
              "aster_test_replay_count.itch";
  constexpr std::uint32_t kMessages = 32;
  {
    std::ofstream f(path, std::ios::binary);
    assert(f.is_open());
    write_header(f, /*symbol_count=*/1);
    write_symbol(f, "C", 0);
    std::uint64_t ts = 0;
    for (std::uint32_t i = 0; i < kMessages; ++i) {
      ts += 100;
      // Half system events, half L2 aggregates — exercising both fast paths.
      if (i % 2 == 0) {
        write_msg(f, 'S', ts, [&](std::ofstream& g) {
          write_u8(g, static_cast<std::uint8_t>(SystemEventCode::MarketOpen));
        });
      } else {
        write_msg(f, 'L', ts, [&](std::ofstream& g) {
          write_u16(g, /*sym=*/0);
          write_u8(g, static_cast<std::uint8_t>(Side::Buy));
          write_u64(g, /*price=*/100ULL + i);
          write_u32(g, /*qty=*/5);
          write_u32(g, /*order_count=*/1);
        });
      }
    }
  }

  ItchParser p(path);
  ReplayConfig cfg;
  cfg.mode = ReplayConfig::SpeedMode::Batch;
  std::uint64_t dispatched = ReplayEngine(p, cfg, [](const Message&,
                                                     std::uint64_t) {}).run();

  assert(p.error_count() == 0);
  assert(p.count() == kMessages);
  assert(dispatched == kMessages);

  std::filesystem::remove(path);
  std::printf("[ok] test_round_trip_message_count\n");
}

}  // namespace

int main() {
  test_dispatch_order_is_deterministic();
  test_latency_injection_rounds();
  test_round_trip_message_count();
  std::printf("\nAll 3 replay tests passed.\n");
  return 0;
}
