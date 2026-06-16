#include "aster/replay_driver.hpp"
#include <algorithm>
#include <thread>

namespace aster {

ReplayDriver::ReplayDriver(MatchingEngine& engine, Config cfg)
    : engine_(engine), cfg_(cfg) {}

void ReplayDriver::load_events(std::vector<OrderEvent> events) {
    events_ = std::move(events);
    if (cfg_.deterministic) {
        std::sort(events_.begin(), events_.end(),
                  [](const OrderEvent& a, const OrderEvent& b) { return a.seq_no < b.seq_no; });
    } else {
        std::sort(events_.begin(), events_.end(),
                  [](const OrderEvent& a, const OrderEvent& b) { return a.timestamp < b.timestamp; });
    }
}

void ReplayDriver::run(std::function<void(const OrderEvent&)> on_pre_latency) {
    if (events_.empty()) return;
    auto& tsc = TscClock::instance();
    Timestamp base_ts = events_.front().timestamp;
    auto wall_start = tsc.now();

    for (size_t i = 0; i < events_.size(); ++i) {
        auto& ev = events_[i];
        if (on_pre_latency) on_pre_latency(ev);

        if (cfg_.replay_speed > 0.0) {
            auto sim_offset = ev.timestamp - base_ts;
            auto target_wall_ticks = wall_start + static_cast<uint64_t>(sim_offset / cfg_.replay_speed);
            while (tsc.now() < target_wall_ticks) {
                _mm_pause();
            }
        }

        uint64_t delay = cfg_.base_latency_ticks;
        if (cfg_.jitter_range_ticks > 0) {
            std::uniform_int_distribution<uint64_t> dist(0, cfg_.jitter_range_ticks);
            delay += dist(rng_);
        }
        if (delay > 0) {
            auto deadline = tsc.now() + delay;
            while (tsc.now() < deadline) _mm_pause();
        }

        if (delayed_handler_) delayed_handler_(ev);

        if (cfg_.mm_reaction_latency_ticks > 0 && post_event_handler_) {
            auto deadline = tsc.now() + cfg_.mm_reaction_latency_ticks;
            while (tsc.now() < deadline) _mm_pause();
        }

        if (post_event_handler_) post_event_handler_(ev);
    }
}

void ReplayDriver::set_delayed_handler(std::function<void(const OrderEvent&)> handler) {
    delayed_handler_ = std::move(handler);
}

void ReplayDriver::set_post_event_handler(std::function<void(const OrderEvent&)> handler) {
    post_event_handler_ = std::move(handler);
}

} // namespace aster
