// Aster — header-only robin_hood flat hash map.
//
// Open addressing, linear probing, backward-shift deletion.
// Fibonacci mixing for 8-byte integral keys. Cache-friendly: all state lives
// in parallel Key/Value vectors + a metadata byte vector.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace aster {

// Metadata stored per slot. One byte — cheap to scan.
enum class SlotState : std::uint8_t { Empty = 0, Occupied = 1, Deleted = 2 };

template <typename K, typename V, typename Hash = std::hash<K>,
          typename KeyEqual = std::equal_to<K>,
          typename Allocator = std::allocator<std::pair<const K, V>>>
class flat_hash_map {
 public:
  using key_type = K;
  using mapped_type = V;
  using value_type = std::pair<const K, V>;
  using size_type = std::size_t;
  using hasher = Hash;
  using key_equal = KeyEqual;

 private:
  // Parallel vectors keep keys and values densely packed.
  using KAlloc =
      typename std::allocator_traits<Allocator>::template rebind_alloc<K>;
  using VAlloc =
      typename std::allocator_traits<Allocator>::template rebind_alloc<V>;
  std::vector<K, KAlloc> keys_;
  std::vector<V, VAlloc> vals_;
  std::vector<SlotState> meta_;
  size_type size_ = 0;       // number of occupied slots
  size_type occupied_ = 0;   // number of occupied + deleted (used for cap calc)
  hasher hash_;
  key_equal eq_;

  static constexpr double kMaxLoad = 0.7;

  static size_type next_pow2(size_type v) {
    // v is assumed > 0.
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
  }

  // For 8-byte integral keys, Fibonacci mixing gives a very good spread for
  // sequential/monotonic keys (OrderID, Price).
  static size_type mix(size_type h) noexcept {
    return static_cast<size_type>(static_cast<std::uint64_t>(h) *
                                  0x9E3779B97F4A7C15ULL);
  }

  size_type hash_key(const K& k) const {
    if constexpr (std::is_integral_v<K> && sizeof(K) == 8) {
      return mix(static_cast<size_type>(k));
    } else {
      return mix(static_cast<size_type>(hash_(k)));
    }
  }

  // Probe for a key. Returns (found, index).
  // If StopOnEmpty is true, returns the first Empty or Deleted slot index
  // where the key could be inserted.
  template <bool StopOnEmpty>
  std::pair<bool, size_type> probe(const K& k) const noexcept {
    const size_type mask = keys_.size() - 1;
    size_type idx = hash_key(k) & mask;
    size_type first_deleted = std::numeric_limits<size_type>::max();
    while (true) {
      SlotState s = meta_[idx];
      if (s == SlotState::Empty) {
        if constexpr (StopOnEmpty) {
          size_type insert_idx =
              (first_deleted != std::numeric_limits<size_type>::max())
                  ? first_deleted
                  : idx;
          return {false, insert_idx};
        }
        idx = (idx + 1) & mask;
        continue;
      }
      if (s == SlotState::Deleted) {
        if (first_deleted == std::numeric_limits<size_type>::max())
          first_deleted = idx;
        idx = (idx + 1) & mask;
        continue;
      }
      // Occupied
      if (eq_(keys_[idx], k)) return {true, idx};
      idx = (idx + 1) & mask;
    }
  }

  void rehash(size_type new_capacity) {
    new_capacity = next_pow2(new_capacity);
    std::vector<K, KAlloc> old_keys(new_capacity);
    std::vector<V, VAlloc> old_vals(new_capacity);
    std::vector<SlotState> old_meta(new_capacity, SlotState::Empty);

    std::swap(keys_, old_keys);
    std::swap(vals_, old_vals);
    std::swap(meta_, old_meta);
    size_ = 0;
    occupied_ = 0;

    for (size_type i = 0, n = old_meta.size(); i < n; ++i) {
      if (old_meta[i] == SlotState::Occupied) {
        insert_unchecked(std::move(old_keys[i]), std::move(old_vals[i]));
      }
    }
  }

  // Insert without rehash into empty slots. Assumes there is room.
  void insert_unchecked(K&& k, V&& v) {
    const size_type mask = keys_.size() - 1;
    size_type idx = hash_key(k) & mask;
    while (meta_[idx] == SlotState::Occupied) {
      idx = (idx + 1) & mask;
    }
    keys_[idx] = std::move(k);
    vals_[idx] = std::move(v);
    meta_[idx] = SlotState::Occupied;
    ++size_;
    ++occupied_;
  }

  void ensure_capacity() {
    if (keys_.size() == 0) {
      rehash(16);
      return;
    }
    if (occupied_ + 1 >= static_cast<size_type>(keys_.size() * kMaxLoad)) {
      rehash(keys_.size() * 2);
    }
  }

 public:
  flat_hash_map() = default;
  ~flat_hash_map() = default;
  flat_hash_map(const flat_hash_map&) = default;
  flat_hash_map& operator=(const flat_hash_map&) = default;
  flat_hash_map(flat_hash_map&&) noexcept = default;
  flat_hash_map& operator=(flat_hash_map&&) noexcept = default;

  // Pre-allocate for `n` elements.
  void reserve(size_type n) {
    size_type cap = static_cast<size_type>(n / kMaxLoad) + 1;
    if (cap <= keys_.size()) return;
    rehash(cap);
  }

  size_type size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  size_type capacity() const noexcept { return keys_.size(); }

  void clear() {
    std::fill(meta_.begin(), meta_.end(), SlotState::Empty);
    size_ = 0;
    occupied_ = 0;
  }

  // Iteration support: expose raw occupied slots. Caller must check meta_.
  const K* keys_data() const noexcept { return keys_.data(); }
  const SlotState* meta_data() const noexcept { return meta_.data(); }
  size_type capacity_raw() const noexcept { return keys_.size(); }

  // Callback-based iteration over occupied slots. Simpler than a full iterator.
  template <typename F>
  void for_each(F&& f) const {
    for (size_type i = 0; i < keys_.size(); ++i) {
      if (meta_[i] == SlotState::Occupied) {
        f(keys_[i], vals_[i]);
      }
    }
  }
  template <typename F>
  void for_each(F&& f) {
    for (size_type i = 0; i < keys_.size(); ++i) {
      if (meta_[i] == SlotState::Occupied) {
        f(keys_[i], vals_[i]);
      }
    }
  }

  // Returns pointer to value, or nullptr. Hot path: no iterator wrapping.
  V* find(const K& k) noexcept {
    if (keys_.empty()) return nullptr;
    auto [ok, idx] = probe<true>(k);
    if (!ok) return nullptr;
    return &vals_[idx];
  }
  const V* find(const K& k) const noexcept {
    if (keys_.empty()) return nullptr;
    auto [ok, idx] = probe<true>(k);
    if (!ok) return nullptr;
    return &vals_[idx];
  }

  bool contains(const K& k) const noexcept {
    return const_cast<flat_hash_map*>(this)->find(k) != nullptr;
  }

  // Inserts {k, V{}} if missing, returns reference to value.
  V& operator[](const K& k) {
    ensure_capacity();
    auto [ok, idx] = probe<true>(k);
    if (ok) return vals_[idx];
    keys_[idx] = k;
    vals_[idx] = V{};
    meta_[idx] = SlotState::Occupied;
    ++size_;
    ++occupied_;
    return vals_[idx];
  }
  V& operator[](K&& k) {
    ensure_capacity();
    auto [ok, idx] = probe<true>(k);
    if (ok) return vals_[idx];
    keys_[idx] = std::move(k);
    vals_[idx] = V{};
    meta_[idx] = SlotState::Occupied;
    ++size_;
    ++occupied_;
    return vals_[idx];
  }

  // Returns true if inserted, false if key already present.
  template <typename KK, typename VV>
  bool insert(KK&& k, VV&& v) {
    ensure_capacity();
    auto [ok, idx] = probe<true>(k);
    if (ok) return false;
    keys_[idx] = std::forward<KK>(k);
    vals_[idx] = std::forward<VV>(v);
    meta_[idx] = SlotState::Occupied;
    ++size_;
    ++occupied_;
    return true;
  }

  // Erases by key. Uses backward-shift deletion to keep probe sequences short.
  bool erase(const K& k) {
    if (keys_.empty()) return false;
    auto [ok, idx] = probe<true>(k);
    if (!ok) return false;
    erase_at(idx);
    --size_;
    --occupied_;
    return true;
  }

 private:
  void erase_at(size_type idx) {
    // Destroy value if non-trivial.
    if constexpr (!std::is_trivially_destructible_v<V>) vals_[idx].~V();
    if constexpr (!std::is_trivially_destructible_v<K>) keys_[idx].~K();

    // Backward-shift deletion: move subsequent occupied slots back into this
    // empty slot until we hit an Empty state.
    const size_type mask = keys_.size() - 1;
    size_type vacant = idx;
    size_type i = (idx + 1) & mask;
    while (meta_[i] != SlotState::Empty) {
      if (meta_[i] == SlotState::Occupied) {
        size_type natural = hash_key(keys_[i]) & mask;
        // Can `i` fill `vacant`? Either natural <= vacant < i (wrap occurred)
        // OR vacant < i < natural (wrap the other way) OR i < natural <= vacant.
        // Equivalent: vacant is NOT in (natural, i].
        bool can_move;
        if (vacant < i) {
          can_move = !(natural > vacant && natural <= i);
        } else {
          can_move = !(natural > vacant || natural <= i);
        }
        if (can_move) {
          keys_[vacant] = std::move(keys_[i]);
          vals_[vacant] = std::move(vals_[i]);
          if constexpr (!std::is_trivially_destructible_v<V>) vals_[i].~V();
          if constexpr (!std::is_trivially_destructible_v<K>) keys_[i].~K();
          meta_[vacant] = SlotState::Occupied;
          meta_[i] = SlotState::Empty;
          vacant = i;
        }
      }
      i = (i + 1) & mask;
    }
    meta_[vacant] = SlotState::Empty;
  }
};

}  // namespace aster
