#pragma once

#include "aster/types.hpp"
#include "aster/order_book.hpp"   // BookUpdate
#include "aster/matching_engine.hpp"
#include <vector>
#include <unordered_map>
#include <deque>
#include <cassert>

namespace aster {

struct MarketMakerConfig {
    double target_inventory = 0.0;
    double max_inventory = 1000.0;
    double skew_factor = 0.0001;        // price skew per unit inventory
    double base_spread = 0.0002;        // 2 bps half-spread
    double order_qty = 10.0;
    double txn_cost = 0.0001;           // 1 bp per unit
    // Fill probability model
    double fill_prob_decay = 0.5;       // lambda for exponential decay per unit queue length
    double min_fill_prob = 0.01;
    // Spread adjustment based on fill prob
    double spread_sensitivity = 0.001;  // extra spread when fill prob low
};

// Stores mid price evolution for markout
struct MidPriceHistory {
    struct Snapshot {
        Timestamp time;
        double mid;
    };
    std::vector<Snapshot> snapshots;

    // Get mid price at or after a given timestamp + offset (in TSC ticks)
    double mid_at(Timestamp ts, Timestamp offset) const;
};

class MarketMakerSimulator {
public:
    MarketMakerSimulator(MatchingEngine& engine,
                         const MarketMakerConfig& cfg,
                         const MidPriceHistory& mid_history);

    // Called *before* each market event is processed (pre-latency).
    // Returns synthetic OrderEvents (cancels & new quotes) to inject.
    std::vector<OrderEvent> on_market_event(const OrderEvent& event,
                                            const std::vector<BookUpdate>& book_snapshot);

    // Called when a trade occurs (to update our position).
    void on_trade(const TradeEvent& trade);

    struct Metrics {
        double pnl = 0.0;
        double sharpe = 0.0;
        double max_drawdown = 0.0;
        double volume_traded = 0.0;
        double adverse_selection_cost = 0.0;   // markout PnL (negative = cost)
    };

    Metrics compute_metrics(double current_mid) const;
    void reset();

private:
    struct Position {
        double net = 0.0;
        double cash = 0.0;
        double peak_equity = 0.0;
        double max_drawdown = 0.0;
        double cumulative_markout = 0.0;   // sum of (fill_price - future_mid) * qty
        void update_fill(double trade_qty, double fill_price, double cost,
                         double markout_price, double markout_qty);
    };

    MarketMakerConfig cfg_;
    MatchingEngine& engine_;
    const MidPriceHistory& mid_history_;
    Position pos_;
    OrderId next_id_ = 1'000'000'000;   // outside replay range

    struct Quote {
        OrderId id;
        Side side;
        Price price;
        Quantity qty;
    };
    std::unordered_map<OrderId, Quote> quotes_;

    double mid_price(const std::vector<BookUpdate>& book) const;

    // Estimate fill probability given our price level and side
    double estimate_fill_prob(const BookUpdate& level, Side our_side,
                             const std::vector<BookUpdate>& book) const;

    // Record a trade event for markout (store fill info)
    struct FillRecord {
        Timestamp ts;
        double price;
        double qty;
        Side aggressor_side; // opposite of our side
    };
    std::vector<FillRecord> fill_records_;
};

} // namespace aster
