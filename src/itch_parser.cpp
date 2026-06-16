#include "aster/itch_parser.hpp"
#include "aster/order_book.hpp"
#include <fstream>
#include <cstring>
#include <unordered_map>
#include <stdexcept>

namespace aster::itch {

#pragma pack(push, 1)
struct AddMsg {
    uint64_t timestamp;
    uint64_t order_id;
    char     symbol[8];
    char     side;
    uint64_t price;
    uint64_t quantity;
};
struct MarketMsg {
    uint64_t timestamp;
    uint64_t order_id;
    char     symbol[8];
    char     side;
    uint64_t quantity;
};
struct CancelMsg {
    uint64_t timestamp;
    uint64_t order_id;
    char     symbol[8];
};
struct ModifyMsg {
    uint64_t timestamp;
    uint64_t order_id;
    char     symbol[8];
    uint64_t new_price;
    uint64_t new_quantity;
};
struct OrderExecutedMsg {
    uint64_t timestamp;
    uint64_t order_id;
    uint64_t traded_quantity;
};
#pragma pack(pop)

std::vector<RawMessage> parse_raw(const std::string& path) {
    std::vector<RawMessage> msgs;
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open ITCH file");

    char type;
    while (file.read(&type, 1)) {
        RawMessage rm;
        rm.type = type;
        size_t size = 0;
        switch (type) {
            case 'A': size = sizeof(AddMsg) - 1; break;
            case 'F': size = sizeof(MarketMsg) - 1; break;
            case 'C': size = sizeof(CancelMsg) - 1; break;
            case 'M': size = sizeof(ModifyMsg) - 1; break;
            case 'E': size = sizeof(OrderExecutedMsg) - 1; break;
            default: continue;
        }
        rm.data.resize(size);
        file.read(rm.data.data(), size);
        msgs.push_back(std::move(rm));
    }
    return msgs;
}

std::vector<OrderEvent> build_event_sequence(const std::string& path,
                                              Timestamp& out_start_ts) {
    auto raw_msgs = parse_raw(path);
    std::vector<OrderEvent> events;
    OrderBook book(1'000'000);

    struct OrderInfo {
        Price price;
        Side side;
        Symbol symbol;
        Quantity current_qty;
    };
    std::unordered_map<OrderId, OrderInfo> info_map;

    Timestamp base_ts = 0;
    uint64_t seq = 0;
    for (const auto& rm : raw_msgs) {
        const char* data = rm.data.data();
        OrderEvent ev;
        ev.event_type = EventType::AddOrder;

        switch (rm.type) {
            case 'A': {
                AddMsg m;
                std::memcpy(&m.timestamp, data, sizeof(m.timestamp));
                std::memcpy(&m.order_id, data+8, sizeof(m.order_id));
                std::memcpy(m.symbol, data+16, 8);
                m.side = *(data+24);
                std::memcpy(&m.price, data+25, sizeof(m.price));
                std::memcpy(&m.quantity, data+33, sizeof(m.quantity));

                ev.timestamp = m.timestamp;
                ev.order_id = m.order_id;
                ev.symbol = std::string(m.symbol, strnlen(m.symbol, 8));
                ev.side = (m.side == 'B') ? Side::Buy : Side::Sell;
                ev.type = OrderType::Limit;
                ev.price = m.price;
                ev.quantity = m.quantity;
                ev.event_type = EventType::AddOrder;

                info_map[m.order_id] = {m.price, ev.side, ev.symbol, m.quantity};
                book.process(ev);
                break;
            }
            case 'F': {
                MarketMsg m;
                std::memcpy(&m.timestamp, data, sizeof(m.timestamp));
                std::memcpy(&m.order_id, data+8, sizeof(m.order_id));
                std::memcpy(m.symbol, data+16, 8);
                m.side = *(data+24);
                std::memcpy(&m.quantity, data+25, sizeof(m.quantity));

                ev.timestamp = m.timestamp;
                ev.order_id = m.order_id;
                ev.symbol = std::string(m.symbol, strnlen(m.symbol, 8));
                ev.side = (m.side == 'B') ? Side::Buy : Side::Sell;
                ev.type = OrderType::Market;
                ev.price = 0;
                ev.quantity = m.quantity;
                ev.event_type = EventType::AddOrder;
                book.process(ev);
                break;
            }
            case 'E': {
                OrderExecutedMsg m;
                std::memcpy(&m.timestamp, data, sizeof(m.timestamp));
                std::memcpy(&m.order_id, data+8, sizeof(m.order_id));
                std::memcpy(&m.traded_quantity, data+16, sizeof(m.traded_quantity));

                auto it = info_map.find(m.order_id);
                if (it == info_map.end()) continue;
                auto& info = it->second;
                if (m.traded_quantity >= info.current_qty) {
                    ev.timestamp = m.timestamp;
                    ev.order_id = m.order_id;
                    ev.symbol = info.symbol;
                    ev.event_type = EventType::OrderCancel;
                    events.push_back(ev);
                    book.cancel_order(info.symbol, m.order_id);
                    info_map.erase(it);
                    continue;
                }
                Quantity new_qty = info.current_qty - m.traded_quantity;
                ev.timestamp = m.timestamp;
                ev.order_id = m.order_id;
                ev.symbol = info.symbol;
                ev.side = info.side;
                ev.type = OrderType::Limit;
                ev.price = info.price;
                ev.quantity = new_qty;
                ev.event_type = EventType::OrderReduce;
                info.current_qty = new_qty;
                book.reduce_order(info.symbol, m.order_id, new_qty);
                break;
            }
            case 'C': {
                CancelMsg m;
                std::memcpy(&m.timestamp, data, sizeof(m.timestamp));
                std::memcpy(&m.order_id, data+8, sizeof(m.order_id));
                std::memcpy(m.symbol, data+16, 8);

                auto it = info_map.find(m.order_id);
                if (it != info_map.end()) {
                    ev.timestamp = m.timestamp;
                    ev.order_id = m.order_id;
                    ev.symbol = std::string(m.symbol, strnlen(m.symbol, 8));
                    ev.event_type = EventType::OrderCancel;
                    book.cancel_order(ev.symbol, m.order_id);
                    info_map.erase(it);
                }
                break;
            }
            case 'M': {
                ModifyMsg m;
                std::memcpy(&m.timestamp, data, sizeof(m.timestamp));
                std::memcpy(&m.order_id, data+8, sizeof(m.order_id));
                std::memcpy(m.symbol, data+16, 8);
                std::memcpy(&m.new_price, data+24, sizeof(m.new_price));
                std::memcpy(&m.new_quantity, data+32, sizeof(m.new_quantity));

                auto it = info_map.find(m.order_id);
                if (it != info_map.end()) {
                    auto& info = it->second;
                    ev.timestamp = m.timestamp;
                    ev.order_id = m.order_id;
                    ev.symbol = std::string(m.symbol, strnlen(m.symbol, 8));
                    ev.side = info.side;
                    ev.type = OrderType::Limit;
                    ev.price = m.new_price;
                    ev.quantity = m.new_quantity;
                    ev.event_type = EventType::OrderModify;
                    book.modify_order(ev.symbol, m.order_id, m.new_quantity, m.new_price, m.timestamp);
                    info.price = m.new_price;
                    info.current_qty = m.new_quantity;
                }
                break;
            }
        }
        ev.seq_no = seq++;
        events.push_back(ev);
        if (base_ts == 0) base_ts = ev.timestamp;
    }
    out_start_ts = base_ts;
    return events;
}

} // namespace aster::itch
