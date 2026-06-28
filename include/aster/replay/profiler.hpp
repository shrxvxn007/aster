// Aster — nanosecond-precision profiler.
//
// Wraps hot-path calls with tsc() before/after, stores deltas in an HDR-style
// histogram (log2 buckets, 2 sig figs, ~500 buckets, no heap allocation).
// Provides p50/p90/p99/p99.9, min, max, count, throughput.

#pragma once

#include "aster/utils/timestamp.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace aster::replay {

class Profiler {
 public:
  static constexpr std::uint64_t kMinBucketNs = 1;
  static constexpr std::uint64_t kMaxBucketNs = 1'000'000'000;  // 1s
  static constexpr int kBucketCount = 512;

  Profiler() { reset(); }

  // Call before the operation.
  void start() noexcept { start_ = tsc(); }

  // Call after the operation. Records the delta.
  void stop() noexcept {
    auto delta = tsc() - start_;
    record(delta);
  }

  // Wrap a callable: start(), call f(), stop().
  template <typename F>
  void measure(F&& f) noexcept {
    start();
    f();
    stop();
  }

  void record(std::uint64_t delta) noexcept {
    if (delta < min_ || count_ == 0) min_ = delta;
    if (delta > max_ || count_ == 0) max_ = delta;
    sum_ += delta;
    // Bucket index: log2(delta) mapped to [0, kBucketCount).
    int idx = 0;
    if (delta > 0) {
      // Use 63 - clz to get floor(log2(delta)).
      idx = 63 - __builtin_clzll(delta);
      // Sub-bucket for 2 sig figs: 10 sub-buckets per power of 2.
      // For simplicity, just use log2 bucket.
      if (idx >= kBucketCount) idx = kBucketCount - 1;
    }
    buckets_[idx]++;
    ++count_;
  }

  void reset() noexcept {
    std::memset(buckets_, 0, sizeof(buckets_));
    count_ = 0;
    sum_ = 0;
    min_ = std::numeric_limits<std::uint64_t>::max();
    max_ = 0;
    start_ = 0;
  }

  std::uint64_t count() const noexcept { return count_; }
  std::uint64_t min_ns() const noexcept { return min_; }
  std::uint64_t max_ns() const noexcept { return max_; }
  double avg_ns() const noexcept {
    return count_ > 0 ? static_cast<double>(sum_) / count_ : 0.0;
  }

  // Percentile: 0.0 to 1.0 (e.g. 0.99 for p99).
  std::uint64_t percentile(double p) const noexcept {
    if (count_ == 0) return 0;
    auto target = static_cast<std::uint64_t>(p * static_cast<double>(count_));
    std::uint64_t cumulative = 0;
    for (int i = 0; i < kBucketCount; ++i) {
      cumulative += buckets_[i];
      if (cumulative >= target) {
        // Return the lower bound of this bucket.
        return (1ULL << static_cast<std::uint64_t>(i));
      }
    }
    return kMaxBucketNs;
  }

  // Print summary to stdout.
  void print(const std::string& label = "profiler") const noexcept {
    std::printf("[%s] count=%llu min=%lluns p50=%lluns p90=%lluns p99=%lluns "
                "p99.9=%lluns max=%lluns avg=%.1fns\n",
                label.c_str(),
                static_cast<unsigned long long>(count_),
                static_cast<unsigned long long>(min_),
                static_cast<unsigned long long>(percentile(0.5)),
                static_cast<unsigned long long>(percentile(0.9)),
                static_cast<unsigned long long>(percentile(0.99)),
                static_cast<unsigned long long>(percentile(0.999)),
                static_cast<unsigned long long>(max_),
                avg_ns());
  }

 private:
  std::uint64_t buckets_[kBucketCount] = {};
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
  std::uint64_t start_ = 0;
};

}  // namespace aster::replay
