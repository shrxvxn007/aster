// Aster — Order struct.
//
// 128-byte aligned: hot half (cache line 0) is touched on every match walk;
// cold half (cache line 1) is touched only on cancel/modify/placement.
// Intrusive doubly-linked list pointers are embedded — no separate node alloc.

#pragma once

#include "types.hpp"

#include <cstdint>

namespace aster {

struct alignas(128) Order {
  // Hot half (cache line 0) — touched on every match walk.
  OrderID order_id = 0;       // 8 bytes
  Price price = 0;            // 8 bytes
  Qty qty_remaining = 0;      // 4 bytes
  SymbolID symbol = 0;        // 2 bytes
  Side side : 1;              // 1 byte
  bool in_book : 1;           // 1 byte
  std::uint8_t pad0 : 6;
  std::uint8_t pad1[21] = {};  // pad to 32 bytes (half a cache line)

  // Cold half (cache line 1) — touched only on cancel/modify/placement.
  Order* next = nullptr;       // 8 bytes
  Order* prev = nullptr;       // 8 bytes
  Qty qty_original = 0;        // 4 bytes
  Timestamp submit_ts = 0;     // 8 bytes
  std::uint32_t pool_index = 0;  // 4 bytes
  std::uint8_t pad2[36] = {};    // pad to 96 bytes (rest of cache line 1)
};
static_assert(sizeof(Order) == 128, "Order must be exactly 128 bytes");
static_assert(alignof(Order) == 128, "Order must be 128-byte aligned");

}  // namespace aster
