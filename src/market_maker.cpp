#include "aster/market_maker.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace aster {

double MidPriceHistory::mid_at(Timestamp ts, Timestamp offset) const {
    if (snapshots.empty()) return 100000.0;
    Timestamp target = ts + offset;
    auto it = std::lower_bound(snapshots.begin(), snapshots.end(), target,
        [](const Snapshot& s, Timestamp t) { return s.time < t; });
    if (it != snapshots.end()) return it->mid;
    return snapshots.back().mid;
}

void MarketMakerSimulator::Position::update_fill(double trade_qty, double fill_price,
                                                  double cost, double markout_price,
                                                  double markout_qty) {
    cash -= trade_qty * fill_price;
    net += trade_qty;
    cash -= cost * std::abs(trade_qty);
    double equity = cash + net * fill_price;
    peak_equity = std::max(peak_equity, equity);
    max_drawdown = std::max(max_drawdown, peak_equity - equity);
    cumulative_markout += (fill_price - markout_price) * markout_qty;
}

MarketMakerSimulator::MarketMakerSimulator(MatchingEngine& engine,
                                           const MarketMakerConfig& cfg,
                                           const MidPriceHistory& mid_history)
    : cfg_(cfg), engine_(engine), mid_history_(mid_history) {}

double MarketMakerSimulator::mid_price(const std::vector<BookUpdate>& book) const {
    Price best_bid = 0, best_ask = INT64_MAX;
    for (auto& u : book) {
        if (u.side == Side::Buy && u.price > best_bid) best_bid = u.price;
        if (u.side == Side::Sell && u.price < best_ask) best_ask = u.price;
    }
    if (best_bid == 0 || best_ask == INT64_MAX) return 100000.0;
    return (best_bid + best_ask) / 2.0;
}

double MarketMakerSimulator::estimate_fill_prob(const BookUpdate& level,
                                                 Side our_side) const {
    double shares_ahead = static_cast<double>(level.total_quantity);
    double avg_qty = state_.avg_trade_size;
    if (avg_qty <= 0.0) avg_qty = 50.0;
    double prob = std::exp(-shares_ahead / avg_qty);
    prob = std::max(prob, cfg_.min_fill_prob);
    return prob;
}

void MarketMakerSimulator::update_state_from_trade(const TradeEvent& trade, double mid_now) {
    double qty = static_cast<double>(trade.quantity);
    state_.avg_trade_size = (1.0 - cfg_.avg_trade_size_ema_alpha) * state_.avg_trade_size
                          + cfg_.avg_trade_size_ema_alpha * qty;

    if (trade.aggressor_side == Side::Buy)
        state_.buy_volume = (1.0 - cfg_.flow_ema_alpha) * state_.buy_volume
                          + cfg_.flow_ema_alpha * qty;
    else
        state_.sell_volume = (1.0 - cfg_.flow_ema_alpha) * state_.sell_volume
                           + cfg_.flow_ema_alpha * qty;

    if (state_.last_mid > 0.0) {
        double ret = std::log(mid_now / state_.last_mid);
        double sq = ret * ret;
        state_.volatility = (1.0 - cfg_.vol_ema_alpha) * state_.volatility
                          + cfg_.vol_ema_alpha * sq;
    }
    state_.last_mid = mid_now;
}

std::vector<OrderEvent> MarketMakerSimulator::on_market_event(const OrderEvent& event,
                                                              const std::vector<BookUpdate>& book) {
    std::vector<OrderEvent> actions;

    // Cancel existing quotes
    for (auto& [id, q] : quotes_) {
        actions.push_back(OrderEvent{
            .timestamp = event.timestamp,
            .seq_no = event.seq_no,
            .order_id = q.id,
            .symbol = event.symbol,
            .side = q.side,
            .type = OrderType::Limit,
            .price = 0,
            .quantity = 0,
            .event_type = EventType::OrderCancel
        });
    }
    quotes_.clear();

    double mid = mid_price(book);
    current_mid_ = mid;
    double inv = pos_.net;

    // Inventory skew
    double skew = -cfg_.skew_factor * (inv - cfg_.target_inventory);

    // Flow imbalance skew
    double total_flow = state_.buy_volume + state_.sell_volume;
    double flow_imbalance = (total_flow > 0.0) ? (state_.buy_volume - state_.sell_volume) / total_flow : 0.0;
    double flow_skew = -cfg_.flow_skew_factor * flow_imbalance;

    double fair = mid * (1.0 + skew + flow_skew);

    // Spread
    double half = cfg_.base_half_spread;
    half += cfg_.vol_spread_factor * std::sqrt(state_.volatility);

    Price bid_p = static_cast<Price>(fair * (1.0 - half));
    Price ask_p = static_cast<Price>(fair * (1.0 + half));

    double prob_bid = 1.0, prob_ask = 1.0;
    for (auto& u : book) {
        if (u.side == Side::Buy && u.price == bid_p)
            prob_bid = estimate_fill_prob(u, Side::Buy);
        if (u.side == Side::Sell && u.price == ask_p)
            prob_ask = estimate_fill_prob(u, Side::Sell);
    }

    Quantity qty = static_cast<Quantity>(cfg_.order_qty);
    if (prob_bid < 0.2) qty = std::max<Quantity>(1, qty / 2);
    if (prob_ask < 0.2) qty = std::max<Quantity>(1, qty / 2);

    last_bid_prob_ = prob_bid;
    last_ask_prob_ = prob_ask;

    OrderId bid_id = next_id_++;
    OrderId ask_id = next_id_++;
    quotes_[bid_id] = {bid_id, Side::Buy, bid_p, qty};
    quotes_[ask_id] = {ask_id, Side::Sell, ask_p, qty};

    actions.push_back(OrderEvent{
        .timestamp = event.timestamp,
        .seq_no = event.seq_no,
        .order_id = bid_id,
        .symbol = event.symbol,
        .side = Side::Buy,
        .type = OrderType::Limit,
        .price = bid_p,
        .quantity = qty,
        .event_type = EventType::AddOrder
    });
    actions.push_back(OrderEvent{
        .timestamp = event.timestamp,
        .seq_no = event.seq_no,
        .order_id = ask_id,
        .symbol = event.symbol,
        .side = Side::Sell,
        .type = OrderType::Limit,
        .price = ask_p,
        .quantity = qty,
        .event_type = EventType::AddOrder
    });

    // Record equity point (mark‑to‑market before this event)
    double equity_before = pos_.cash + pos_.net * mid;
    equity_curve_.push_back({event.timestamp, equity_before});

    return actions;
}

void MarketMakerSimulator::on_trade(const TradeEvent& trade) {
    double mid_now = mid_history_.mid_at(trade.timestamp, 0);
    if (mid_now == 0.0) mid_now = 100000.0;
    update_state_from_trade(trade, mid_now);

    auto it = quotes_.find(trade.resting_id);
    if (it != quotes_.end()) {
        double cost = cfg_.txn_cost;
        double qty = static_cast<double>(trade.quantity);
        double price = static_cast<double>(trade.price);
        FillRecord rec{trade.timestamp, price, qty,
                       (trade.aggressor_side == Side::Buy) ? Side::Buy : Side::Sell};
        fill_records_.push_back(rec);

        if (trade.aggressor_side == Side::Buy) {
            pos_.update_fill(-qty, price, cost, 0, 0);
        } else {
            pos_.update_fill(qty, price, cost, 0, 0);
        }
        quotes_.erase(it);
    }
}

MarketMakerSimulator::Metrics MarketMakerSimulator::compute_metrics(double current_mid,
                                                                    double interval_us) const {
    double ns_per_tick = TscClock::instance().tsc_to_ns(1);
    Timestamp one_ms_ticks = static_cast<uint64_t>(1e6 / ns_per_tick);
    return compute_metrics_with_horizon(current_mid, one_ms_ticks, interval_us);
}

MarketMakerSimulator::Metrics MarketMakerSimulator::compute_metrics_with_horizon(
        double current_mid, Timestamp markout_ticks, double interval_us) const {
    Metrics m;
    m.pnl = pos_.cash + pos_.net * current_mid;
    m.max_drawdown = pos_.max_drawdown / pos_.peak_equity;
    m.volume_traded = std::abs(pos_.net);

    double total_markout_pnl = 0.0;
    for (auto& rec : fill_records_) {
        double fut_mid = mid_history_.mid_at(rec.ts, markout_ticks);
        double markout = (rec.aggressor_side == Side::Buy)
                         ? (rec.price - fut_mid) * rec.qty
                         : (fut_mid - rec.price) * rec.qty;
        total_markout_pnl += markout;
    }
    m.adverse_selection_cost = -total_markout_pnl;
    m.pnl += total_markout_pnl;

    // Sharpe ratio
    if (equity_curve_.size() >= 2) {
        std::vector<double> returns;
        double ns_per_tick = TscClock::instance().tsc_to_ns(1);
        for (size_t i = 1; i < equity_curve_.size(); ++i) {
            double eq_prev = equity_curve_[i-1].equity;
            double eq_curr = equity_curve_[i].equity;
            if (eq_prev > 0) {
                double ret = std::log(eq_curr / eq_prev);
                double dt_days = 0.0;
                if (interval_us > 0.0) {
                    dt_days = interval_us * 1e-6 / 86400.0;
                } else {
                    dt_days = (equity_curve_[i].time - equity_curve_[i-1].time) * ns_per_tick / 8.64e13;
                }
                if (dt_days > 0.0) {
                    double daily_ret = ret / dt_days;
                    returns.push_back(daily_ret);
                }
            }
        }
        if (!returns.empty()) {
            double mean = 0.0, var = 0.0;
            for (double r : returns) mean += r;
            mean /= returns.size();
            for (double r : returns) var += (r - mean) * (r - mean);
            var /= returns.size();
            double daily_std = std::sqrt(var);
            if (daily_std > 0.0)
                m.sharpe = (mean / daily_std) * std::sqrt(252);
        }
    }

    return m;
}

void MarketMakerSimulator::reset() {
    pos_ = Position{};
    state_ = State{};
    quotes_.clear();
    fill_records_.clear();
    equity_curve_.clear();
}

// Helper: build mid price history by replaying events
MidPriceHistory build_mid_history(const std::vector<OrderEvent>& events,
                                  MatchingEngine& engine) {
    MidPriceHistory hist;
    engine.clear();
    for (auto& ev : events) {
        engine.process(ev);
        std::vector<BookUpdate> snap;
        engine.snapshot(ev.symbol, snap, ev.timestamp);
        Price best_bid = 0, best_ask = INT64_MAX;
        for (auto& u : snap) {
            if (u.side == Side::Buy && u.price > best_bid) best_bid = u.price;
            if (u.side == Side::Sell && u.price < best_ask) best_ask = u.price;
        }
        double mid = (best_bid && best_ask != INT64_MAX) ? (best_bid + best_ask) / 2.0 : 100000.0;
        hist.snapshots.push_back({ev.timestamp, mid});
    }
    engine.clear();
    return hist;
}

} // namespace aster
