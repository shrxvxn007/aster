// Aster — OrderPool.
//
// Pre-allocated contiguous vector of Orders with an intrusive free-list of
// pool indices. acquire() and release() are O(1) and never touch the heap.

#pragma once

#include "order.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

#if defined(__linux__)
#  include <sys/mman.h>
#elif defined(__APPLE__)
#  include <mach/mach.h>
#endif

namespace aster {

class OrderPool {
 public:
  explicit OrderPool(std::uint32_t capacity)
      : pool_(capacity), free_head_(0), size_(0) {
    // Build the free-list: each slot's pool_index points to the next free slot.
    for (std::uint32_t i = 0; i + 1 < capacity; ++i) {
      pool_[i].pool_index = i + 1;
      pool_[i].in_book = false;
    }
    pool_[capacity - 1].pool_index = kInvalid;
    pool_[capacity - 1].in_book = false;

    // Hint the kernel that we'll stream through this buffer sequentially.
    // Reduces TLB misses during replay. No-op on macOS (no hugepage ABI)
    // and on non-Linux platforms.
#if defined(__linux__)
    auto* addr = reinterpret_cast<void*>(pool_.data());
    auto bytes = pool_.size() * sizeof(Order);
    if (bytes > 0) {
      // MADV_SEQUENTIAL: expect sequential access; prefetch aggressively.
      // MADV_HUGETLB would require explicit hugepage allocation (and may
      // fail); MADV_SEQUENTIAL is the safe, always-succeeds middle ground.
      madvise(addr, bytes, MADV_SEQUENTIAL);
    }
#endif
  }

  // Returns nullptr if the pool is exhausted.
  Order* acquire() noexcept {
    if (free_head_ == kInvalid) return nullptr;
    Order* o = &pool_[free_head_];
    free_head_ = o->pool_index;
    o->in_book = false;
    o->next = o->prev = nullptr;
    ++size_;
    return o;
  }

  void release(Order* o) noexcept {
    assert(o != nullptr);
    assert(o >= pool_.data() && o < pool_.data() + pool_.size());
    o->in_book = false;
    o->next = o->prev = nullptr;
    o->pool_index = free_head_;
    free_head_ = static_cast<std::uint32_t>(o - pool_.data());
    --size_;
  }

  Order* by_index(std::uint32_t i) { return &pool_[i]; }
  const Order* by_index(std::uint32_t i) const { return &pool_[i]; }

  std::uint32_t size() const noexcept { return size_; }
  std::uint32_t capacity() const noexcept {
    return static_cast<std::uint32_t>(pool_.size());
  }

 private:
  static constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;
  std::vector<Order> pool_;
  std::uint32_t free_head_;
  std::uint32_t size_;
};

}  // namespace aster
