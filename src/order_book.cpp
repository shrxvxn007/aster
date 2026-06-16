#include "aster/order_book.hpp"
#include <cassert>
#include <iostream>

namespace aster {

// ============ HalfBook template implementation ============

template<Side S>
HalfBook<S>::HalfBook(OrderPool& pool) : pool_(pool) {
    levels_.reserve(128);
}

template<Side S>
void HalfBook<S>::add(Order* order) {
    auto it = std::lower_bound(levels_.begin(), levels_.end(), order->price,
        [](const PriceLevel& lvl, Price p) {
            if constexpr (S == Side::Buy) return lvl.price > p;
            else return lvl.price < p;
        });
    if (it == levels_.end() || it->price != order->price) {
        it = levels_.insert(it, PriceLevel{order->price, {}});
    }
    it->orders.push_back(order);
    order_lookup_[order->id] = order;
}

template<Side S>
Order* HalfBook<S>::cancel(OrderId id) {
    auto it = order_lookup_.find(id);
    if (it == order_lookup_.end()) return nullptr;
    Order* ord = it->second;
    int idx = find_level(ord->price);
    assert(idx >= 0);
    auto& lvl = levels_[idx];
    lvl.orders.remove(ord);
    if (lvl.orders.empty()) {
        levels_.erase(levels_.begin() + idx);
    }
    order_lookup_.erase(it);
    return ord;
}

template<Side S>
bool HalfBook<S>::modify(OrderId id, Quantity new_qty, Price new_price, Timestamp new_ts) {
    auto it = order_lookup_.find(id);
    if (it == order_lookup_.end()) return false;
    Order* ord = it->second;
    Order* removed = cancel(id);
    (void)removed;
    ord->qty = new_qty;
    ord->price = new_price;
    ord->timestamp = new_ts;
    add(ord);
    return true;
}

template<Side S>
bool HalfBook<S>::reduce(OrderId id, Quantity new_qty) {
    auto it = order_lookup_.find(id);
    if (it == order_lookup_.end()) return false;
    Order* ord = it->second;
    if (new_qty >= ord->qty) {
        cancel(id);
        pool_.deallocate(ord);
        return true;
    }
    ord->qty = new_qty;
    return true;
}

template<Side S>
std::optional<Price> HalfBook<S>::best_price() const {
    if (levels_.empty()) return std::nullopt;
    return levels_[0].price;
}

template<Side S>
Quantity HalfBook<S>::match(Quantity aggress_qty, Price limit_price,
                             std::vector<TradeEvent>& trades,
                             std::vector<Order*>& filled_orders,
                             OrderId aggressor_id, Symbol symbol, Timestamp now) {
    Quantity filled = 0;
    auto it = levels_.begin();
    while (it != levels_.end() && filled < aggress_qty) {
        if constexpr (S == Side::Sell) {
            if (it->price > limit_price) break;
        } else {
            if (it->price < limit_price) break;
        }
        auto& list = it->orders;
        while (!list.empty() && filled < aggress_qty) {
            Order* rest = list.front();
            Quantity fill_qty = std::min(aggress_qty - filled, rest->qty);
            rest->qty -= fill_qty;
            filled += fill_qty;

            trades.push_back(TradeEvent{
                .timestamp = now,
                .resting_id = rest->id,
                .aggressor_id = aggressor_id,
                .symbol = symbol,
                .price = rest->price,
                .quantity = fill_qty,
                .aggressor_side = (S == Side::Sell ? Side::Buy : Side::Sell)
            });

            if (rest->qty == 0) {
                list.remove(rest);
                order_lookup_.erase(rest->id);
                filled_orders.push_back(rest);
            } else {
                break;
            }
        }
        if (list.empty()) {
            it = levels_.erase(it);
        } else {
            ++it;
        }
    }
    return filled;
}

template<Side S>
void HalfBook<S>::snapshot(std::vector<BookUpdate>& updates, Symbol symbol, Timestamp now) const {
    for (const auto& lvl : levels_) {
        Quantity total_qty = 0;
        const Order* node = lvl.orders.front();
        uint32_t count = 0;
        while (node) {
            total_qty += node->qty;
            ++count;
            node = node->next;
        }
        updates.push_back(BookUpdate{
            .timestamp = now,
            .symbol = symbol,
            .side = S,
            .price = lvl.price,
            .total_quantity = total_qty,
            .order_count = count
        });
    }
}

template<Side S>
int HalfBook<S>::find_level(Price price) const {
    auto it = std::lower_bound(levels_.begin(), levels_.end(), price,
        [](const PriceLevel& lvl, Price p) {
            if constexpr (S == Side::Buy) return lvl.price > p;
            else return lvl.price < p;
        });
    if (it != levels_.end() && it->price == price)
        return static_cast<int>(it - levels_.begin());
    return -1;
}

template<Side S>
void HalfBook<S>::clear() {
    for (auto& lvl : levels_) {
        Order* node = lvl.orders.front();
        while (node) {
            Order* next = node->next;
            pool_.deallocate(node);
            node = next;
        }
    }
    levels_.clear();
    order_lookup_.clear();
}

template class HalfBook<Side::Buy>;
template class HalfBook<Side::Sell>;

// ================== OrderBook ====================

OrderBook::OrderBook(size_t pool_capacity) : pool_(pool_capacity) {}

void OrderBook::add_order(Order order) {
    Order* ptr = pool_.allocate();
    if (!ptr) {
        static bool warned = false;
        if (!warned) {
            std::cerr << "Warning: OrderPool exhausted, dropping orders.\n";
            warned = true;
        }
        return;
    }
    *ptr = order;
    ptr->next = ptr->prev = nullptr;
    auto& sym_books = books_.try_emplace(order.symbol, pool_).first->second;
    if (order.side == Side::Buy)
        sym_books.buys.add(ptr);
    else
        sym_books.sells.add(ptr);
}

bool OrderBook::cancel_order(const Symbol& sym, OrderId id) {
    auto it = books_.find(sym);
    if (it == books_.end()) return false;
    Order* removed = it->second.buys.cancel(id);
    if (!removed) removed = it->second.sells.cancel(id);
    if (removed) {
        pool_.deallocate(removed);
        return true;
    }
    return false;
}

bool OrderBook::modify_order(const Symbol& sym, OrderId id, Quantity new_qty, Price new_price,
                              Timestamp new_ts) {
    auto it = books_.find(sym);
    if (it == books_.end()) return false;
    if (it->second.buys.modify(id, new_qty, new_price, new_ts)) return true;
    return it->second.sells.modify(id, new_qty, new_price, new_ts);
}

bool OrderBook::reduce_order(const Symbol& sym, OrderId id, Quantity new_qty) {
    auto it = books_.find(sym);
    if (it == books_.end()) return false;
    if (it->second.buys.reduce(id, new_qty)) return true;
    return it->second.sells.reduce(id, new_qty);
}

std::vector<TradeEvent> OrderBook::process(const OrderEvent& event) {
    std::vector<TradeEvent> trades;
    auto sym_it = books_.find(event.symbol);
    if (sym_it == books_.end()) {
        if (event.event_type == EventType::AddOrder || event.event_type == EventType::OrderReduce)
            books_.try_emplace(event.symbol, pool_);
        else return trades;
    }
    auto& sym = books_[event.symbol];

    if (event.event_type == EventType::AddOrder) {
        if (event.type == OrderType::Market) {
            std::vector<Order*> filled;
            Price limit = (event.side == Side::Buy) ? Price(INT64_MAX) : Price(0);
            if (event.side == Side::Buy)
                sym.sells.match(event.quantity, limit, trades, filled,
                                event.order_id, event.symbol, event.timestamp);
            else
                sym.buys.match(event.quantity, limit, trades, filled,
                               event.order_id, event.symbol, event.timestamp);
            for (Order* o : filled) pool_.deallocate(o);
        } else {
            Quantity remaining = event.quantity;
            std::vector<Order*> filled;
            if (event.side == Side::Buy)
                remaining -= sym.sells.match(remaining, event.price, trades, filled,
                                             event.order_id, event.symbol, event.timestamp);
            else
                remaining -= sym.buys.match(remaining, event.price, trades, filled,
                                            event.order_id, event.symbol, event.timestamp);
            for (Order* o : filled) pool_.deallocate(o);
            if (remaining > 0) {
                Order order{event.order_id, event.price, remaining, event.timestamp, event.side};
                add_order(order);
            }
        }
    }
    else if (event.event_type == EventType::OrderCancel) {
        cancel_order(event.symbol, event.order_id);
    }
    else if (event.event_type == EventType::OrderModify) {
        modify_order(event.symbol, event.order_id, event.quantity, event.price, event.timestamp);
    }
    else if (event.event_type == EventType::OrderReduce) {
        reduce_order(event.symbol, event.order_id, event.quantity);
    }

    return trades;
}

void OrderBook::snapshot(const Symbol& sym, std::vector<BookUpdate>& updates, Timestamp now) const {
    auto it = books_.find(sym);
    if (it == books_.end()) return;
    it->second.buys.snapshot(updates, sym, now);
    it->second.sells.snapshot(updates, sym, now);
}

void OrderBook::clear() {
    for (auto& [_, sym] : books_) {
        sym.buys.clear();
        sym.sells.clear();
    }
    books_.clear();
}

OrderPool& OrderBook::pool() { return pool_; }

} // namespace aster
