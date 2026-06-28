// Aster — core types.
//
// All prices are fixed-point uint64_t scaled by 1e5 (5 decimal places).
// No floating-point on the critical path.
//
// Layout rationale:
//   - Price/Qty/OrderID/SymbolID/Timestamp are defined here so every subsystem
//     (core, strategy, replay) shares one definition.
//   - ExecutionReport is padded to exactly 64 bytes (one cache line) so a
//     vector of reports is dense in cache and prefetcher-friendly.
//   - SymbolTable is cold-path only (intern at startup, lookup at output).

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aster {

// ---------------------------------------------------------------------------
// Scalar types
// ---------------------------------------------------------------------------

using Price = std::uint64_t;     // fixed-point, scaled 1e5
using Qty = std::uint32_t;
using OrderID = std::uint64_t;
using SymbolID = std::uint16_t;
using Timestamp = std::uint64_t;  // nanoseconds since epoch

// ---------------------------------------------------------------------------
// Price helpers
// ---------------------------------------------------------------------------

constexpr Price kPriceScale = 100'000ULL;
constexpr Price kPriceZero = 0;
constexpr Price kPriceInvalid = UINT64_MAX;

constexpr Qty kQtyInvalid = 0xFFFFFFFFu;

// I/O helpers only — never used in the matching path.
inline Price from_double(double v) noexcept {
  return static_cast<Price>(v * static_cast<double>(kPriceScale) + 0.5);
}
inline double to_price(Price p) noexcept {
  return static_cast<double>(p) / static_cast<double>(kPriceScale);
}

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

inline const char* side_name(Side s) noexcept {
  return s == Side::Buy ? "Buy" : "Sell";
}

// Event types emitted via ExecutionReport.
enum class EventType : std::uint8_t {
  Fill,
  Accept,
  Cancel,
  Modify,
  Reject,
  System
};

// ---------------------------------------------------------------------------
// ExecutionReport — exactly 64 bytes (one cache line).
//
// Layout (with typical alignment):
//   offset  size  field
//   0       2     symbol
//   2       6     pad
//   8       8     order_id
//   16      8     counterparty
//   24      8     price
//   32      4     qty
//   36      4     pad
//   40      1     side (bitfield)
//   40      3     type (bitfield, shares byte with side)
//   40      4     pad (remaining bits)
//   48      8     timestamp
//   56      8     recv_timestamp
//
// Transient: produced on the hot path, consumed by the callback.
// ---------------------------------------------------------------------------

struct ExecutionReport {
  SymbolID symbol = 0;        // 2 bytes
  std::uint8_t pad0[6] = {};  // align to 8
  OrderID order_id = 0;       // 8 bytes
  OrderID counterparty = 0;    // 8 bytes
  Price price = 0;            // 8 bytes
  Qty qty = 0;                // 4 bytes
  std::uint8_t pad1[4] = {};  // align to 8
  Side side : 1;              // 1 byte (packed)
  EventType type : 3;         // 1 byte (packed)
  std::uint8_t pad2 : 4;
  std::uint8_t pad3[5] = {};  // align to 8
  Timestamp timestamp = 0;    // 8 bytes (exchange time)
  Timestamp recv_timestamp = 0;  // 8 bytes (after latency injection)
};
static_assert(sizeof(ExecutionReport) == 64,
              "ExecutionReport must be exactly 64 bytes");

// ---------------------------------------------------------------------------
// SymbolTable — interns string <-> SymbolID. Cold path only (startup + output).
// ---------------------------------------------------------------------------

class SymbolTable {
 public:
  SymbolID intern(std::string_view name) {
    auto it = rev_.find(name);
    if (it != rev_.end()) return it->second;
    SymbolID id = static_cast<SymbolID>(names_.size());
    names_.emplace_back(name);
    std::string_view stored(names_.back());
    rev_[stored] = id;
    return id;
  }

  std::string_view lookup(SymbolID id) const {
    return names_[static_cast<std::size_t>(id)];
  }

  std::size_t size() const { return names_.size(); }

  void reserve(std::size_t n) {
    names_.reserve(n);
    rev_.reserve(n);
  }

 private:
  std::vector<std::string> names_;
  std::unordered_map<std::string_view, SymbolID> rev_;
};

}  // namespace aster
