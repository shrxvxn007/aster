#pragma once

#include <atomic>
#include <vector>
#include <cstddef>

namespace aster {

constexpr size_t CACHELINE = 64;

template <typename T>
class SPSCQueue {
public:
    explicit SPSCQueue(size_t capacity)
        : capacity_(capacity + 1)
        , buffer_(capacity + 1)
    {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    bool push(const T& item) {
        size_t cur_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (cur_tail + 1) % capacity_;
        if (next_tail == head_.load(std::memory_order_acquire))
            return false;
        buffer_[cur_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    size_t push_batch(const T* items, size_t count) {
        size_t pushed = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!push(items[i])) break;
            ++pushed;
        }
        return pushed;
    }

    bool pop(T& item) {
        size_t cur_head = head_.load(std::memory_order_relaxed);
        if (cur_head == tail_.load(std::memory_order_acquire))
            return false;
        item = buffer_[cur_head];
        head_.store((cur_head + 1) % capacity_, std::memory_order_release);
        return true;
    }

    size_t pop_batch(T* items, size_t max_count) {
        size_t popped = 0;
        for (size_t i = 0; i < max_count; ++i) {
            if (!pop(items[i])) break;
            ++popped;
        }
        return popped;
    }

private:
    alignas(CACHELINE) std::atomic<size_t> head_;
    char pad0_[CACHELINE - sizeof(head_)];

    alignas(CACHELINE) std::atomic<size_t> tail_;
    char pad1_[CACHELINE - sizeof(tail_)];

    const size_t capacity_;
    std::vector<T> buffer_;
    char pad2_[CACHELINE - sizeof(capacity_) % CACHELINE];
};

} // namespace aster
