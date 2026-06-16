#pragma once

#include "aster/order.hpp"
#include "aster/types.hpp"
#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm>

namespace aster {

template<Side S>
class HalfBook {
public:
    explicit HalfBook(OrderPool& pool);

    void add(Order* order);
    Order* cancel(OrderId id);
    bool modify(OrderId id, Quantity new_qty, Price new_price, Timestamp new_ts);
    bool reduce(OrderId id, Quantity new_qty);

    std::optional<Price> best_price() const;

    Quantity match(Quantity aggress_qty, Price limit_price,
                   std::vector<TradeEvent>& trades,
                   std::vector<Order*>& filled_orders,
                   OrderId aggressor_id, Symbol symbol, Timestamp now);

    void snapshot(std::vector<BookUpdate>& updates, Symbol symbol, Timestamp now) const;
    size_t total_orders() const { return order_lookup_.size(); }
    void clear();

private:
    PriceLevelList levels_;                     // intrusive sorted list of price levels
    std::unordered_map<Price, PriceLevel*> price_map_; // O(1) price -> level
    OrderPool& pool_;
    std::unordered_map<OrderId, Order*> order_lookup_;

    // helper: create a new PriceLevel and insert into levels_ and price_map_
    PriceLevel* create_level(Price price);
};

class OrderBook {
public:
    explicit OrderBook(size_t pool_capacity);

    void add_order(Order order);
    bool cancel_order(const Symbol& sym, OrderId id);
    bool modify_order(const Symbol& sym, OrderId id, Quantity new_qty, Price new_price,
                      Timestamp new_ts);
    bool reduce_order(const Symbol& sym, OrderId id, Quantity new_qty);

    std::vector<TradeEvent> process(const OrderEvent& event);
    void snapshot(const Symbol& sym, std::vector<BookUpdate>& updates, Timestamp now) const;
    void clear();

    OrderPool& pool() { return pool_; }

private:
    struct SymbolBooks {
        HalfBook<Side::Buy>  buys;
        HalfBook<Side::Sell> sells;
        SymbolBooks(OrderPool& p) : buys(p), sells(p) {}
    };

    std::unordered_map<Symbol, SymbolBooks> books_;
    OrderPool pool_;
};

} // namespace aster
