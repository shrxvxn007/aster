#pragma once

#include <cstdint>
#include <chrono>
#include <thread>

#ifdef __x86_64__
#include <x86intrin.h>
#endif

namespace aster {

inline uint64_t rdtsc() {
#ifdef __x86_64__
    unsigned int aux;
    return __rdtscp(&aux);
#else
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
#endif
}

class TscClock {
public:
    static TscClock& instance() {
        static TscClock c;
        return c;
    }

    double tsc_to_ns(uint64_t tsc) const { return tsc * ns_per_tick_; }
    uint64_t now() const { return rdtsc(); }

private:
    double ns_per_tick_;

    TscClock() {
        constexpr auto calib_duration = std::chrono::milliseconds(100);
        auto t1 = rdtsc();
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < calib_duration) {
            // spin
        }
        auto t2 = rdtsc();
        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(calib_duration).count();
        ns_per_tick_ = static_cast<double>(elapsed_ns) / (t2 - t1);
    }
};

} // namespace aster
