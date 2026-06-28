// Aster — timestamp helpers.
// Hot-path safe: now_ns() and tsc() are markable for inlining everywhere.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#if defined(__x86_64__) || defined(_M_X64)
#  include <x86intrin.h>
#endif

namespace aster {

// Nanoseconds since an arbitrary epoch (steady clock). Suitable for latency
// math: never goes backwards, but not wall-clock aligned (don't persist it).
inline std::uint64_t now_ns() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::high_resolution_clock::now().time_since_epoch())
          .count());
}

// Highest-resolution cycle counter available on this platform.
// On x86-64: rdtsc. On ARM64 (Apple Silicon, etc.): chrono steady_clock
// (no direct userspace CNTVCT_EL0 without a kernel quirk on macOS).
inline std::uint64_t tsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
#  if defined(__APPLE__)
    // Apple Rosetta and bare-metal both expose rdtsc; use the intrinsic.
    return __rdtsc();
#  else
    return __rdtsc();
#  endif
#else
  // Fallback: still high-resolution on modern OS (1ns granularity typical).
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
#endif
}

// Cold path: format a nanosecond timestamp as a human-readable string.
// Not for use on the critical path.
inline std::string format_ns(std::uint64_t ns) {
  std::time_t sec = static_cast<std::time_t>(ns / 1'000'000'000ULL);
  auto nsec = static_cast<long>(ns % 1'000'000'000ULL);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &sec);
#else
  localtime_r(&sec, &tm_buf);
#endif
  char out[64];
  std::size_t n = std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%S", &tm_buf);
  if (n > 0) {
    int w = std::snprintf(out + n, sizeof(out) - n, ".%09ld", nsec);
    if (w > 0) return std::string(out, n + static_cast<std::size_t>(w));
  }
  return std::to_string(ns);
}

}  // namespace aster
