#pragma once

#include "aster/types.hpp"
#include "aster/order_book.hpp"
#include "aster/matching_engine.hpp"
#include <vector>
#include <unordered_map>
#include <deque>
#include <cassert>

namespace aster {

struct MarketMakerConfig {
    double target_inventory = 0.0;
    double max_inventory = 1000.0;
    double skew_factor = 0.0001;          // price skew per unit inventory
    double base_half_spread = 0.0001;     // 1 bp half‑spread
    double order_qty = 10.0;
    double txn_cost = 0.0001;             // 1 bp per unit

    // Fill probability model
    double avg_trade_size_ema_alpha = 0.01;   // EMA for trade size
    double min_fill_prob = 0.01;

    // Volatility adjustment
    double vol_ema_alpha = 0.001;
    double vol_spread_factor = 1.0;        // extra half‑spread = factor * vol

    // Order flow imbalance
    double flow_ema_alpha = 0.01;
    double flow_skew_factor = 0.00005;     // shift fair price per unit imbalance
};

struct MidPriceHistory {
    struct Snapshot {
        Timestamp time;
        double mid;
    };
    std::vector<Snapshot> snapshots;

    double mid_at(Timestamp ts, Timestamp offset) const;
};

class MarketMakerSimulator {
public:
    MarketMakerSimulator(MatchingEngine& engine,
                         const MarketMakerConfig& cfg,
                         const MidPriceHistory& mid_history);

    std::vector<OrderEvent> on_market_event(const OrderEvent& event,
                                            const std::vector<BookUpdate>& book);

    void on_trade(const TradeEvent& trade);

    std::pair<double, double> get_last_quote_probs() const {
        return {last_bid_prob_, last_ask_prob_};
    }

    struct Metrics {
        double pnl = 0.0;
        double sharpe = 0.0;
        double max_drawdown = 0.0;
        double volume_traded = 0.0;
        double adverse_selection_cost = 0.0;
    };

    Metrics compute_metrics(double current_mid, double interval_us = 0.0) const;
    Metrics compute_metrics_with_horizon(double current_mid, Timestamp markout_ticks,
                                         double interval_us = 0.0) const;
    void reset();

private:
    struct Position {
        double net = 0.0;
        double cash = 0.0;
        double peak_equity = 0.0;
        double max_drawdown = 0.0;
        double cumulative_markout = 0.0;
        void update_fill(double trade_qty, double fill_price, double cost,
                         double markout_price, double markout_qty);
    };

    struct State {
        double avg_trade_size = 50.0;
        double volatility = 0.0;
        double buy_volume = 0.0;
        double sell_volume = 0.0;
        double last_mid = 0.0;
    };

    MarketMakerConfig cfg_;
    MatchingEngine& engine_;
    const MidPriceHistory& mid_history_;
    Position pos_;
    State state_;
    OrderId next_id_ = 1'000'000'000;

    struct Quote {
        OrderId id;
        Side side;
        Price price;
        Quantity qty;
    };
    std::unordered_map<OrderId, Quote> quotes_;

    double current_mid_ = 0.0;
    double mid_price(const std::vector<BookUpdate>& book) const;
    double estimate_fill_prob(const BookUpdate& level, Side our_side) const;

    double last_bid_prob_ = 1.0;
    double last_ask_prob_ = 1.0;

    struct EquityPoint {
        Timestamp time;
        double equity;
    };
    mutable std::vector<EquityPoint> equity_curve_;

    struct FillRecord {
        Timestamp ts;
        double price;
        double qty;
        Side aggressor_side;
    };
    std::vector<FillRecord> fill_records_;

    void update_state_from_trade(const TradeEvent& trade, double mid_now);
};

MidPriceHistory build_mid_history(const std::vector<OrderEvent>& events,
                                  MatchingEngine& engine);

} // namespace aster
