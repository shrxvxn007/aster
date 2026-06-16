#include "aster/latency_profiler.hpp"
#include <iostream>
#include <format>

namespace aster {

void LatencyProfiler::record(uint64_t start_tsc, uint64_t end_tsc) {
    double ns = TscClock::instance().tsc_to_ns(end_tsc - start_tsc);
    latencies_ns_.push_back(ns);
}

LatencyProfiler::Stats LatencyProfiler::compute() const {
    if (latencies_ns_.empty()) return {};
    std::vector<double> sorted = latencies_ns_;
    std::sort(sorted.begin(), sorted.end());
    size_t n = sorted.size();
    Stats s;
    s.avg_ns = std::accumulate(sorted.begin(), sorted.end(), 0.0) / n;
    s.p50_ns = sorted[n/2];
    s.p99_ns = sorted[static_cast<size_t>(n * 0.99)];
    s.p999_ns = sorted[static_cast<size_t>(n * 0.999)];
    s.max_ns = sorted.back();
    return s;
}

void LatencyProfiler::report() const {
    auto s = compute();
    std::cout << std::format("Latency stats: avg={:.1f} ns, p50={:.0f} ns, p99={:.0f} ns, "
                             "p999={:.0f} ns, max={:.0f} ns\n",
                             s.avg_ns, s.p50_ns, s.p99_ns, s.p999_ns, s.max_ns);
}

} // namespace aster
