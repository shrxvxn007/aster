#include "aster/itch_parser.hpp"
#include "aster/matching_engine.hpp"
#include "aster/replay_driver.hpp"
#include "aster/latency_profiler.hpp"
#include "aster/market_maker.hpp"
#include "aster/tsc_clock.hpp"
#include <iostream>
#include <format>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cmath>

struct ProbBin {
    double lo, hi;
    size_t placed = 0;
    size_t filled = 0;
};

static std::vector<ProbBin> prob_bins = {
    {0.0, 0.05}, {0.05, 0.15}, {0.15, 0.30}, {0.30, 0.50},
    {0.50, 0.70}, {0.70, 0.85}, {0.85, 0.95}, {0.95, 1.01}
};

void record_prediction(double pred, bool filled) {
    for (auto& b : prob_bins) {
        if (pred >= b.lo && pred < b.hi) {
            b.placed++;
            if (filled) b.filled++;
            return;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: aster <itch_file> [--speed <mult>] [--latency <ns>] [--jitter <ns>]\n"
                     "              [--benchmark] [--replay] [--mm-latency <ns>] [--sharpe-interval <us>]\n";
        return 1;
    }
    std::string itch_file = argv[1];
    double speed = 0.0, lat_ns = 0.0, jit_ns = 0.0, mm_lat_ns = 0.0, sharpe_interval_us = 0.0;
    bool bench = false, replay = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--speed") == 0 && i+1<argc) speed = atof(argv[++i]);
        else if (strcmp(argv[i], "--latency") == 0 && i+1<argc) lat_ns = atof(argv[++i]);
        else if (strcmp(argv[i], "--jitter") == 0 && i+1<argc) jit_ns = atof(argv[++i]);
        else if (strcmp(argv[i], "--benchmark") == 0) bench = true;
        else if (strcmp(argv[i], "--replay") == 0) replay = true;
        else if (strcmp(argv[i], "--mm-latency") == 0 && i+1<argc) mm_lat_ns = atof(argv[++i]);
        else if (strcmp(argv[i], "--sharpe-interval") == 0 && i+1<argc) sharpe_interval_us = atof(argv[++i]);
    }

    auto& tsc = aster::TscClock::instance();
    double ns_per_tick = tsc.tsc_to_ns(1);
    auto ns_to_ticks = [&](double ns) -> uint64_t { return static_cast<uint64_t>(ns / ns_per_tick); };

    aster::Timestamp start_ts;
    auto events = aster::itch::build_event_sequence(itch_file, start_ts);
    std::cout << std::format("Loaded {} events\n", events.size());

    aster::MatchingEngine engine(3'000'000);
    if (bench) {
        aster::LatencyProfiler profiler;
        auto t0 = tsc.now();
        for (auto& ev : events) {
            auto t1 = tsc.now();
            engine.process(ev);
            auto t2 = tsc.now();
            profiler.record(t1, t2);
        }
        auto t1 = tsc.now();
        double total_sec = tsc.tsc_to_ns(t1 - t0) * 1e-9;
        std::cout << std::format("Benchmark: {} events in {:.3f}s ({:.0f} orders/s)\n",
                                 events.size(), total_sec, events.size()/total_sec);
        profiler.report();
        return 0;
    }

    auto mid_history = aster::build_mid_history(events, engine);
    aster::MarketMakerConfig mm_cfg;
    aster::MarketMakerSimulator mm(engine, mm_cfg, mid_history);

    // for fill probability validation
    std::unordered_map<aster::OrderId, double> pred_map;
    engine.on_trade([&](const aster::TradeEvent& t) {
        mm.on_trade(t);
        auto it = pred_map.find(t.resting_id);
        if (it != pred_map.end()) {
            record_prediction(it->second, true);
            pred_map.erase(it);
        }
    });

    aster::LatencyProfiler profiler;

    if (replay) {
        aster::ReplayDriver::Config rcfg;
        rcfg.replay_speed = speed;
        rcfg.base_latency_ticks = ns_to_ticks(lat_ns);
        rcfg.jitter_range_ticks = ns_to_ticks(jit_ns);
        rcfg.mm_reaction_latency_ticks = ns_to_ticks(mm_lat_ns);

        aster::ReplayDriver driver(engine, rcfg);
        driver.load_events(events);

        driver.set_delayed_handler([&](const aster::OrderEvent& ev) {
            auto t1 = tsc.now();
            engine.process(ev);
            auto t2 = tsc.now();
            profiler.record(t1, t2);
        });

        driver.set_post_event_handler([&](const aster::OrderEvent& ev) {
            std::vector<aster::BookUpdate> book;
            engine.snapshot(ev.symbol, book, ev.timestamp);
            auto actions = mm.on_market_event(ev, book);
            auto [bid_p, ask_p] = mm.get_last_quote_probs();
            for (auto& act : actions) {
                act.timestamp = ev.timestamp;
                act.seq_no = ev.seq_no;
                engine.process(act);
                if (act.event_type == aster::EventType::AddOrder) {
                    if (act.side == aster::Side::Buy) pred_map[act.order_id] = bid_p;
                    else pred_map[act.order_id] = ask_p;
                } else if (act.event_type == aster::EventType::OrderCancel) {
                    pred_map.erase(act.order_id);
                }
            }
        });

        driver.run([](const aster::OrderEvent&) {});   // pre‑latency nothing
    } else {
        for (size_t i = 0; i < events.size(); ++i) {
            const auto& ev = events[i];
            // 1. Process market event
            auto t1 = tsc.now();
            engine.process(ev);
            auto t2 = tsc.now();
            profiler.record(t1, t2);

            // 2. Snapshot
            std::vector<aster::BookUpdate> book;
            engine.snapshot(ev.symbol, book, ev.timestamp);

            // 3. MM reacts
            auto actions = mm.on_market_event(ev, book);
            auto [bid_p, ask_p] = mm.get_last_quote_probs();
            for (auto& act : actions) {
                act.timestamp = ev.timestamp;
                act.seq_no = ev.seq_no;
                engine.process(act);
                if (act.event_type == aster::EventType::AddOrder) {
                    if (act.side == aster::Side::Buy) pred_map[act.order_id] = bid_p;
                    else pred_map[act.order_id] = ask_p;
                } else if (act.event_type == aster::EventType::OrderCancel) {
                    pred_map.erase(act.order_id);
                }
            }
        }
    }

    // fill probability validation
    for (auto& [id, prob] : pred_map) {
        record_prediction(prob, false);
    }
    std::cout << "\nFill-probability validation (predicted vs actual):\n";
    std::cout << std::format("{:<8} {:<8} {:<8} {:<12}\n", "Bin", "Placed", "Filled", "Actual%");
    for (auto& bin : prob_bins) {
        if (bin.placed == 0) continue;
        double actual = 100.0 * bin.filled / bin.placed;
        std::cout << std::format("[{:0.2f}-{:0.2f}) {:>6} {:>6} {:>10.1f}%\n",
                                 bin.lo, bin.hi, bin.placed, bin.filled, actual);
    }

    profiler.report();

    double final_mid = mid_history.snapshots.back().mid;
    auto metrics = mm.compute_metrics(final_mid, sharpe_interval_us);
    std::cout << std::format("MM PnL: {:.2f}, Sharpe: {:.3f}, MaxDD: {:.4f}, Vol: {:.2f}\n",
                             metrics.pnl, metrics.sharpe, metrics.max_drawdown, metrics.volume_traded);

    std::cout << "\nAdverse selection vs markout horizon:\n";
    auto ns_to_ticks_lambda = [&](double ns) { return static_cast<uint64_t>(ns / ns_per_tick); };
    for (double horizon_ns : {1e5, 1e6, 10e6, 100e6}) {
        auto m = mm.compute_metrics_with_horizon(final_mid, ns_to_ticks_lambda(horizon_ns), sharpe_interval_us);
        std::cout << std::format("  {:>8.0f} µs  adv_sel_cost = {:+.2f}\n", horizon_ns/1e3, m.adverse_selection_cost);
    }

    return 0;
}
