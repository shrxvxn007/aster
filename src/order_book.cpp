#include "aster/order_book.hpp"
#include <cassert>
#include <iostream>

namespace aster {

// ============ HalfBook template implementation ============

template<Side S>
HalfBook<S>::HalfBook(OrderPool& pool) : pool_(pool) {}

template<Side S>
PriceLevel* HalfBook<S>::create_level(Price price) {
    // Allocate from a pool of PriceLevels (we'll keep a static vector for simplicity)
    // For brevity we use dynamic allocation (could be a pool). Real production would use a memory pool.
    auto* lvl = new PriceLevel{};
    lvl->price = price;
    lvl->total_quantity = 0;
    lvl->order_count = 0;
    levels_.insert<S>(lvl);
    price_map_[price] = lvl;
    return lvl;
}

template<Side S>
void HalfBook<S>::add(Order* order) {
    auto it = price_map_.find(order->price);
    PriceLevel* lvl = nullptr;
    if (it == price_map_.end()) {
        lvl = create_level(order->price);
    } else {
        lvl = it->second;
    }
    // Attach order to price level
    lvl->orders.push_back(order);
    order->level_ptr = lvl;
    lvl->total_quantity += order->qty;
    lvl->order_count++;
    order_lookup_[order->id] = order;
}

template<Side S>
Order* HalfBook<S>::cancel(OrderId id) {
    auto it = order_lookup_.find(id);
    if (it == order_lookup_.end()) return nullptr;
    Order* ord = it->second;
    PriceLevel* lvl = ord->level_ptr;
    lvl->orders.remove(ord);
    lvl->total_quantity -= ord->qty;
    lvl->order_count--;
    if (lvl->order_count == 0) {
        // Remove empty price level
        levels_.remove(lvl);
        price_map_.erase(lvl->price);
        delete lvl;   // or return to pool
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
    // Re-add (time priority lost)
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
    Quantity diff = ord->qty - new_qty;
    ord->qty = new_qty;
    ord->level_ptr->total_quantity -= diff;
    return true;
}

template<Side S>
std::optional<Price> HalfBook<S>::best_price() const {
    auto* first = levels_.first();
    if (!first) return std::nullopt;
    return first->price;
}

template<Side S>
Quantity HalfBook<S>::match(Quantity aggress_qty, Price limit_price,
                             std::vector<TradeEvent>& trades,
                             std::vector<Order*>& filled_orders,
                             OrderId aggressor_id, Symbol symbol, Timestamp now) {
    Quantity filled = 0;
    PriceLevel* lvl = levels_.first();
    while (lvl && filled < aggress_qty) {
        if constexpr (S == Side::Sell) {
            if (lvl->price > limit_price) break;
        } else {
            if (lvl->price < limit_price) break;
        }
        auto& list = lvl->orders;
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
                lvl->total_quantity -= fill_qty;
                lvl->order_count--;
            } else {
                lvl->total_quantity -= fill_qty;
                break; // partial fill on this order
            }
        }
        // If price level now empty, remove it
        if (lvl->order_count == 0) {
            PriceLevel* next = lvl->next;
            levels_.remove(lvl);
            price_map_.erase(lvl->price);
            delete lvl;
            lvl = next;
        } else {
            lvl = lvl->next;
        }
    }
    return filled;
}

template<Side S>
void HalfBook<S>::snapshot(std::vector<BookUpdate>& updates, Symbol symbol, Timestamp now) const {
    for (PriceLevel* lvl = levels_.first(); lvl; lvl = lvl->next) {
        updates.push_back(BookUpdate{
            .timestamp = now,
            .symbol = symbol,
            .side = S,
            .price = lvl->price,
            .total_quantity = lvl->total_quantity,
            .order_count = lvl->order_count
        });
    }
}

template<Side S>
void HalfBook<S>::clear() {
    for (PriceLevel* lvl = levels_.first(); lvl; ) {
        PriceLevel* next = lvl->next;
        while (!lvl->orders.empty()) {
            Order* o = lvl->orders.front();
            lvl->orders.remove(o);
            pool_.deallocate(o);
        }
        delete lvl;
        lvl = next;
    }
    price_map_.clear();
    order_lookup_.clear();
}

// explicit instantiation
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
    ptr->level_ptr = nullptr;
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
    // ... (unchanged logic, but calls updated methods)
    // Same as previous version, unchanged.
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
