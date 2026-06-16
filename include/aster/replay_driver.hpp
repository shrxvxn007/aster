#pragma once

#include "aster/types.hpp"
#include "aster/matching_engine.hpp"
#include "aster/tsc_clock.hpp"
#include <vector>
#include <random>
#include <functional>

namespace aster {

class ReplayDriver {
public:
    struct Config {
        double replay_speed = 1.0;
        bool deterministic = true;
        uint64_t base_latency_ticks = 0;
        uint64_t jitter_range_ticks = 0;
        uint64_t mm_reaction_latency_ticks = 0;   // additional delay for MM post‑event
    };

    ReplayDriver(MatchingEngine& engine, Config cfg);
    void load_events(std::vector<OrderEvent> events);
    void run(std::function<void(const OrderEvent&)> on_pre_latency);

    void set_delayed_handler(std::function<void(const OrderEvent&)> handler);
    void set_post_event_handler(std::function<void(const OrderEvent&)> handler);

private:
    MatchingEngine& engine_;
    Config cfg_;
    std::vector<OrderEvent> events_;
    std::function<void(const OrderEvent&)> delayed_handler_;
    std::function<void(const OrderEvent&)> post_event_handler_;
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace aster
