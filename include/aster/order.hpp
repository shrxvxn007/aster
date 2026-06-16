#pragma once

#include "aster/types.hpp"
#include <vector>
#include <memory>

namespace aster {

struct OrderNode {
    OrderNode* next = nullptr;
    OrderNode* prev = nullptr;
};

// Forward declare PriceLevel (will be defined later)
struct PriceLevel;

struct Order : public OrderNode {
    OrderId     id;
    Price       price;
    Quantity    qty;
    Timestamp   timestamp;
    Side        side;
    PriceLevel* level_ptr = nullptr;   // O(1) access to containing price level
    Order*      pool_next = nullptr;
};

// Intrusive list node for price levels
struct PriceLevelNode {
    PriceLevelNode* next = nullptr;
    PriceLevelNode* prev = nullptr;
};

struct PriceLevel : public PriceLevelNode {
    Price price;
    IntrusiveList orders;          // orders at this price
    Quantity total_quantity = 0;   // incremental aggregate
    uint32_t order_count = 0;      // incremental aggregate
};

class OrderPool {
public:
    explicit OrderPool(size_t capacity) {
        pool_ = std::make_unique<Order[]>(capacity);
        for (size_t i = 0; i < capacity - 1; ++i) {
            pool_[i].pool_next = &pool_[i + 1];
        }
        pool_[capacity - 1].pool_next = nullptr;
        free_list_ = &pool_[0];
    }

    Order* allocate() {
        if (!free_list_) return nullptr;
        Order* obj = free_list_;
        free_list_ = free_list_->pool_next;
        obj->next = obj->prev = nullptr;
        obj->level_ptr = nullptr;
        return obj;
    }

    void deallocate(Order* obj) {
        obj->pool_next = free_list_;
        free_list_ = obj;
    }

private:
    std::unique_ptr<Order[]> pool_;
    Order* free_list_ = nullptr;
};

class IntrusiveList {
public:
    IntrusiveList() : head_(nullptr), tail_(nullptr) {}

    bool empty() const { return head_ == nullptr; }
    Order* front() const { return head_; }
    Order* back()  const { return tail_; }

    void push_back(Order* node) {
        node->next = nullptr;
        node->prev = tail_;
        if (tail_) {
            tail_->next = node;
        } else {
            head_ = node;
        }
        tail_ = node;
    }

    void remove(Order* node) {
        if (node->prev) node->prev->next = node->next;
        else head_ = node->next;
        if (node->next) node->next->prev = node->prev;
        else tail_ = node->prev;
    }
};

// Intrusive list for PriceLevel nodes, sorted by price
class PriceLevelList {
public:
    PriceLevelList() : head_(nullptr), tail_(nullptr) {}

    // Insert level in price order (buy: descending, sell: ascending)
    template<Side S>
    void insert(PriceLevel* level) {
        PriceLevel* cur = head_;
        PriceLevel* prev = nullptr;
        while (cur) {
            if constexpr (S == Side::Buy) {
                if (level->price > cur->price) break;
            } else {
                if (level->price < cur->price) break;
            }
            prev = cur;
            cur = cur->next;
        }
        // insert between prev and cur
        level->next = cur;
        level->prev = prev;
        if (prev) prev->next = level;
        else head_ = level;
        if (cur) cur->prev = level;
        else tail_ = level;
    }

    void remove(PriceLevel* level) {
        if (level->prev) level->prev->next = level->next;
        else head_ = level->next;
        if (level->next) level->next->prev = level->prev;
        else tail_ = level->prev;
    }

    PriceLevel* first() const { return head_; }
    bool empty() const { return head_ == nullptr; }

private:
    PriceLevel* head_;
    PriceLevel* tail_;
};

} // namespace aster
