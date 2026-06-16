#include "aster/itch_parser.hpp"
#include "aster/matching_engine.hpp"
#include "aster/latency_profiler.hpp"
#include "aster/spsc_queue.hpp"
#include "aster/tsc_clock.hpp"
#include <thread>
#include <atomic>
#include <iostream>
#include <format>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: multithreaded_aster <itch_file>\n";
        return 1;
    }
    std::string itch_file = argv[1];
    aster::Timestamp start_ts;
    auto events = aster::itch::build_event_sequence(itch_file, start_ts);
    std::cout << std::format("Loaded {} events\n", events.size());

    aster::SPSCQueue<aster::OrderEvent> queue(4096);
    aster::MatchingEngine engine(2'000'000);
    std::atomic<bool> done{false};
    auto& tsc = aster::TscClock::instance();

    std::thread producer([&]() {
        for (auto& ev : events) {
            while (!queue.push(ev)) {
                _mm_pause();
            }
        }
        done = true;
    });

    aster::LatencyProfiler profiler;
    uint64_t t_start = tsc.now();

    std::thread consumer([&]() {
        aster::OrderEvent ev;
        while (true) {
            if (queue.pop(ev)) {
                auto t1 = tsc.now();
                engine.process(ev);
                auto t2 = tsc.now();
                profiler.record(t1, t2);
            } else {
                if (done.load(std::memory_order_acquire)) break;
                _mm_pause();
            }
        }
        while (queue.pop(ev)) {
            auto t1 = tsc.now();
            engine.process(ev);
            auto t2 = tsc.now();
            profiler.record(t1, t2);
        }
    });

    producer.join();
    consumer.join();

    uint64_t t_end = tsc.now();
    double total_sec = tsc.tsc_to_ns(t_end - t_start) * 1e-9;
    std::cout << std::format("Multi-threaded throughput: {:.0f} orders/s\n", events.size() / total_sec);
    profiler.report();
    return 0;
}
