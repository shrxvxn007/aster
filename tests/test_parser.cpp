// Aster — parser tests.
//
// ItchParser mmaps its input on disk, so each test materialises a small
// ITCH-style binary file in std::filesystem::temp_directory_path() before
// parsing and removes it afterwards. Tests cover the file header + symbol
// table, every message type ('S','A','E','C','D','L'), deterministic
// dispatch order across a stream, and error_count() on a truncated input.

#include "aster/replay/itch.hpp"
#include "aster/replay/parser.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <variant>
#include <vector>

using namespace aster;
using namespace aster::replay;

namespace {

// Append `n` bytes from `src` to the file stream (helpers for big-endian
// encoding with no endianness dependency in the test).
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
  // 7 bytes big-endian — ITCH timestamp width.
  for (int i = 6; i >= 0; --i) write_u8(f, static_cast<std::uint8_t>(v >> (i * 8)));
}
void write_u64(std::ofstream& f, std::uint64_t v) {
  for (int i = 7; i >= 0; --i) write_u8(f, static_cast<std::uint8_t>(v >> (i * 8)));
}

// Write the fixed 12-byte ITCH file header (magic + version + symbol_count).
void write_header(std::ofstream& f, std::uint32_t symbol_count) {
  f.put('I'); f.put('T'); f.put('C'); f.put('H');  // magic
  write_u32(f, /*version=*/1);
  write_u32(f, symbol_count);
}

// Write the symbol-table entry: name_len(1) + name(name_len) + sym_id(2).
void write_symbol(std::ofstream& f, const std::string& name, std::uint16_t id) {
  write_u8(f, static_cast<std::uint8_t>(name.size()));
  f.write(name.data(), static_cast<std::streamsize>(name.size()));
  write_u16(f, id);
}

// Write a 1-byte type tag and 7-byte timestamp; then any payload via
// the caller's lambda. Centralises the prefix so each test only writes the
// payload it cares about.
template <typename PayloadFn>
void write_msg(std::ofstream& f, char type, std::uint64_t ts, PayloadFn payload) {
  write_u8(f, static_cast<std::uint8_t>(type));
  write_u48_be(f, ts);
  payload(f);
}

// -------------------------------------------------------------------------

void test_header_and_symbol_table() {
  auto path = std::filesystem::temp_directory_path() / "aster_test_symtab.itch";
  {
    std::ofstream f(path, std::ios::binary);
    assert(f.is_open());
    write_header(f, /*symbol_count=*/2);
    write_symbol(f, "AAPL", 0);
    write_symbol(f, "MSFT", 1);
    // Truncate here — we're only testing the header & symbol table parse.
  }

  ItchParser p(path);
  assert(p.is_open());
  assert(p.error_count() == 0);
  const auto& syms = p.symbols();
  assert(syms.size() == 2);
  assert(syms[0].name == "AAPL" && syms[0].id == 0);
  assert(syms[1].name == "MSFT" && syms[1].id == 1);
  std::printf("[ok] test_header_and_symbol_table\n");

  std::filesystem::remove(path);
}

void test_add_message_roundtrip() {
  auto path = std::filesystem::temp_directory_path() / "aster_test_add.itch";
  {
    std::ofstream f(path, std::ios::binary);
    assert(f.is_open());
    write_header(f, /*symbol_count=*/1);
    write_symbol(f, "FUT", 7);
    write_msg(f, 'A', /*ts=*/1000, [&](std::ofstream& g) {
      write_u64(g, /*order_id=*/42);
      write_u16(g, /*sym=*/7);
      write_u8(g, /*side=*/static_cast<std::uint8_t>(Side::Buy));
      write_u64(g, /*price=*/100'000ULL);
      write_u32(g, /*qty=*/5);
    });
  }
  ItchParser p(path);
  Message m;
  assert(p.next(m));
  assert(p.count() == 1);
  assert(p.error_count() == 0);

  const OrderAddMsg* a = std::get_if<OrderAddMsg>(&m);
  assert(a != nullptr);
  assert(a->timestamp == 1000);
  assert(a->order_id == 42);
  assert(a->symbol == 7);
  assert(a->side == Side::Buy);
  assert(a->price == 100'000ULL);
  assert(a->qty == 5);
  assert(!p.next(m));  // EOF
  std::printf("[ok] test_add_message_roundtrip\n");
  std::filesystem::remove(path);
}

void test_full_message_stream_in_order() {
  // Build a small stream of every supported message type and verify they
  // come back in the order they were written. This is the determinism
  // guarantee that the ReplayEngine relies on.
  auto path = std::filesystem::temp_directory_path() / "aster_test_stream.itch";
  {
    std::ofstream f(path, std::ios::binary);
    write_header(f, /*symbol_count=*/1);
    write_symbol(f, "ABC", 0);

    write_msg(f, 'S', 10, [&](std::ofstream& g) {
      write_u8(g, static_cast<std::uint8_t>(SystemEventCode::MarketOpen));
    });
    write_msg(f, 'A', 20, [&](std::ofstream& g) {
      write_u64(g, /*oid=*/1);
      write_u16(g, 0);
      write_u8(g, static_cast<std::uint8_t>(Side::Buy));
      write_u64(g, 100ULL);
      write_u32(g, 10);
    });
    write_msg(f, 'E', 30, [&](std::ofstream& g) {
      write_u64(g, /*oid=*/1);
      write_u32(g, /*qty=*/4);
    });
    write_msg(f, 'C', 40, [&](std::ofstream& g) {
      write_u64(g, /*oid=*/1);
      write_u32(g, /*qty=*/2);
    });
    write_msg(f, 'D', 50, [&](std::ofstream& g) { write_u64(g, /*oid=*/1); });
    write_msg(f, 'L', 60, [&](std::ofstream& g) {
      write_u16(g, /*sym=*/0);
      write_u8(g, static_cast<std::uint8_t>(Side::Sell));
      write_u64(g, /*price=*/200ULL);
      write_u32(g, /*qty=*/15);
      write_u32(g, /*order_count=*/3);
    });
    write_msg(f, 'S', 70, [&](std::ofstream& g) {
      write_u8(g, static_cast<std::uint8_t>(SystemEventCode::MarketClose));
    });
  }

  ItchParser p(path);
  std::vector<Message> out;
  Message m;
  while (p.next(m)) out.push_back(m);
  assert(p.error_count() == 0);
  assert(out.size() == 7);

  // Verify the type-tag of every message by index.
  assert(std::holds_alternative<SystemEventMsg>(out[0]));
  assert(std::get<SystemEventMsg>(out[0]).code == SystemEventCode::MarketOpen);
  assert(std::holds_alternative<OrderAddMsg>(out[1]));
  assert(std::holds_alternative<OrderExecuteMsg>(out[2]));
  assert(std::get<OrderExecuteMsg>(out[2]).qty == 4);
  assert(std::holds_alternative<OrderCancelMsg>(out[3]));
  assert(std::get<OrderCancelMsg>(out[3]).qty == 2);
  assert(std::holds_alternative<OrderDeleteMsg>(out[4]));
  assert(std::holds_alternative<L2AggregateMsg>(out[5]));
  const auto& l2 = std::get<L2AggregateMsg>(out[5]);
  assert(l2.price == 200ULL);
  assert(l2.qty == 15);
  assert(l2.order_count == 3);
  assert(std::holds_alternative<SystemEventMsg>(out[6]));
  assert(std::get<SystemEventMsg>(out[6]).code == SystemEventCode::MarketClose);

  // Timestamps strictly increasing — defensive: the parser never zeros them.
  for (std::size_t i = 0; i < out.size(); ++i) {
    Timestamp t = std::visit([](const auto& x) { return x.timestamp; }, out[i]);
    assert(t == 10ULL * (i + 1));
  }
  std::printf("[ok] test_full_message_stream_in_order\n");
  std::filesystem::remove(path);
}

void test_truncated_input_increments_error_count() {
  // A header advertising N symbols but a file that ends mid-message should
  // yield error_count() > 0 (no exceptions — the parser is for hot iteration).
  auto path = std::filesystem::temp_directory_path() / "aster_test_trunc.itch";
  {
    std::ofstream f(path, std::ios::binary);
    write_header(f, /*symbol_count=*/1);
    write_symbol(f, "X", 0);
    // Truncate mid-message: write a type tag + timestamp but no payload.
    write_u8(f, 'A');
    write_u48_be(f, 99ULL);
    // ... no payload bytes; file ends here.
  }
  ItchParser p(path);
  Message m;
  assert(p.next(m) == false);
  assert(p.error_count() == 1);
  std::printf("[ok] test_truncated_input_increments_error_count\n");
  std::filesystem::remove(path);
}

}  // namespace

int main() {
  test_header_and_symbol_table();
  test_add_message_roundtrip();
  test_full_message_stream_in_order();
  test_truncated_input_increments_error_count();
  std::printf("\nAll %d parser tests passed.\n", 4);
  return 0;
}
