#pragma once

#include <cstdint>
#include <chrono>
#include <string>

namespace aster {

using OrderId    = uint64_t;
using Price      = int64_t;       // fixed-point, e.g. 1e-8 scaling
using Quantity   = uint64_t;
using Symbol     = std::string;
using Timestamp  = std::chrono::nanoseconds;   // from epoch or simulation start

enum class Side : uint8_t { Buy, Sell };

enum class OrderType : uint8_t { Limit, Market };

enum class EventType : uint8_t {
    AddOrder,
    OrderExecuted,
    OrderCancel,
    OrderModify,
    Trade,
    BookUpdate
};

// An input event that drives the matching engine (used in replay)
struct OrderEvent {
    Timestamp   timestamp;        // simulated exchange timestamp
    uint64_t    seq_no;           // global deterministic sequence number
    OrderId     order_id;
    Symbol      symbol;
    Side        side;
    OrderType   type;
    Price       price;
    Quantity    quantity;
    EventType   event_type;       // Add, Cancel, Modify
};

// Output events
struct TradeEvent {
    Timestamp   timestamp;
    OrderId     resting_id;
    OrderId     aggressor_id;
    Symbol      symbol;
    Price       price;
    Quantity    quantity;
    Side        aggressor_side;   // side that triggered the match
};

struct BookUpdate {
    Timestamp   timestamp;
    Symbol      symbol;
    Side        side;
    Price       price;
    Quantity    total_quantity;   // total at that price level
    uint32_t    order_count;      // number of orders at that level (L3)
};

} // namespace aster
