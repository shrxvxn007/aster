// Aster — Aster-flavour ITCH wire format.
//
// Binary, big-endian, fixed-size messages. File layout:
//   [magic:4][version:4][symbol_count:4]
//   [symbol table: symbol_count entries of (name_len:1 + name + symbol_id:2)]
//   [message stream: variable-length messages]
//
// Message types (first byte is the type discriminator):
//   'S' = System Event
//   'A' = Order Add (L3)
//   'E' = Order Execute (L3)
//   'C' = Order Cancel (L3)
//   'D' = Order Delete (L3)
//   'L' = L2 Aggregate (price-level depth update)
//
// This format is a deliberate, documented subset of NASDAQ ITCH 5.0 —
// NOT a byte-for-byte clone. Departures from the NASDAQ spec:
//   * Timestamps are 7 bytes (56-bit) big-endian.
//     NASDAQ ITCH 5.0: 6 bytes (48-bit) nanoseconds since midnight.
//   * Symbols are referenced by a 2-byte SymbolID via the file-header
//     symbol table (compact, multi-symbol friendly).
//     NASDAQ ITCH 5.0: 8-byte StockLocate + 'R' Stock Directory messages
//     carrying an 8-byte ASCII Stock name per symbol, dispatched inline.
//   * A single 'A' Order Add carries the price (limit-order only).
//     NASDAQ ITCH 5.0: 'A' is a market-order Add; price lives in a
//     separate 'F' Order Add with MPID attribution message.
//   * An 'L' L2 Aggregate carries price-level total qty + order count
//     for fast depth reconciliation. NASDAQ ITCH 5.0 has no equivalent
//     — its wire format is L3-only (Message-By-Order, MBO).
//   * Six message types total: 'S','A','E','C','D','L'. NASDAQ ITCH 5.0
//     defines 20+ types (cross trades, broken trades, NOII, Reg-SHO,
//     Stock Trading Action, etc.); only the six above are needed by
//     this matching engine.

#pragma once

#include "aster/core/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aster::replay {

// File header.
struct FileHeader {
  char magic[4] = {'I', 'T', 'C', 'H'};
  std::uint32_t version = 1;
  std::uint32_t symbol_count = 0;
};

// Symbol table entry.
struct SymbolEntry {
  SymbolID id;
  std::string name;
};

// System event codes.
enum class SystemEventCode : std::uint8_t {
  MarketOpen = 'O',
  MarketClose = 'C',
  Halt = 'H',
  Resume = 'R',
};

// Messages.
struct SystemEventMsg {
  Timestamp timestamp;
  SystemEventCode code;
};

struct OrderAddMsg {
  Timestamp timestamp;
  OrderID order_id;
  SymbolID symbol;
  Side side;
  Price price;
  Qty qty;
};

struct OrderExecuteMsg {
  Timestamp timestamp;
  OrderID order_id;
  Qty qty;
};

struct OrderCancelMsg {
  Timestamp timestamp;
  OrderID order_id;
  Qty qty;  // qty cancelled (partial cancel supported)
};

struct OrderDeleteMsg {
  Timestamp timestamp;
  OrderID order_id;
};

// L2 aggregate: a price-level depth update. Reports the total quantity
// resting at a given price on one side. Used for L2 book reconstruction
// without replaying every L3 event.
struct L2AggregateMsg {
  Timestamp timestamp;
  SymbolID symbol;
  Side side;
  Price price;
  Qty qty;       // total qty at this price level
  std::uint32_t order_count;  // number of orders at this level
};

using Message = std::variant<SystemEventMsg, OrderAddMsg, OrderExecuteMsg,
                             OrderCancelMsg, OrderDeleteMsg, L2AggregateMsg>;

}  // namespace aster::replay
