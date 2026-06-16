#pragma once

#include "aster/types.hpp"
#include <vector>
#include <memory>

namespace aster {

struct OrderNode {
    OrderNode* next = nullptr;
    OrderNode* prev = nullptr;
};

struct Order : public OrderNode {
    OrderId     id;
    Price       price;
    Quantity    qty;
    Timestamp   timestamp;
    Side        side;
    Order*      pool_next = nullptr;   // free list pointer
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

    void transfer_to(IntrusiveList& other) {
        if (empty()) return;
        if (other.empty()) {
            other.head_ = head_;
            other.tail_ = tail_;
        } else {
            other.tail_->next = head_;
            head_->prev = other.tail_;
            other.tail_ = tail_;
        }
        head_ = tail_ = nullptr;
    }

private:
    Order* head_;
    Order* tail_;
};

} // namespace aster
