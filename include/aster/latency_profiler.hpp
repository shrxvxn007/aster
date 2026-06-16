#pragma once

#include "aster/tsc_clock.hpp"
#include <vector>
#include <algorithm>
#include <numeric>

namespace aster {

class LatencyProfiler {
public:
    void record(uint64_t start_tsc, uint64_t end_tsc);

    struct Stats {
        double avg_ns, p50_ns, p99_ns, p999_ns, max_ns;
    };
    Stats compute() const;
    void report() const;

private:
    std::vector<double> latencies_ns_;
};

} // namespace aster
