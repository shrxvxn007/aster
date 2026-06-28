// Aster — Analytics implementation.

#include "aster/strategy/analytics.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace aster::strategy {

void Analytics::ensure_symbol(SymbolID sym) {
  if (sym >= symbols_.size()) {
    symbols_.resize(static_cast<std::size_t>(sym) + 1);
  }
}

void Analytics::on_fill(const ExecutionReport& r, bool is_agent_taker,
                        Timestamp ts) {
  ensure_symbol(r.symbol);
  auto& s = symbols_[static_cast<std::size_t>(r.symbol)];
  double price = to_price(r.price);
  double qty_d = static_cast<double>(r.qty);
  double fee = qty_d * (is_agent_taker ? config_.taker_fee_per_share
                                       : config_.maker_fee_per_share);
  s.fees += fee;
  total_fees_ += fee;

  // Inventory update: buy = +qty, sell = -qty.
  std::int64_t delta = (r.side == Side::Buy) ? static_cast<std::int64_t>(r.qty)
                                              : -static_cast<std::int64_t>(r.qty);
  std::int64_t old_inv = s.inventory;
  std::int64_t new_inv = old_inv + delta;

  // Realized PnL: if we're reducing position (sign change or same direction
  // reduction), realize the closed portion.
  if ((old_inv > 0 && delta < 0) || (old_inv < 0 && delta > 0)) {
    // Closing trade.
    double closed_qty = static_cast<double>(
        std::min(std::abs(delta), std::abs(old_inv)));
    double pnl = 0.0;
    if (old_inv > 0) {
      // Was long, now selling: realize (sell_price - avg_entry) * closed_qty.
      pnl = (price - s.avg_entry) * closed_qty;
    } else {
      // Was short, now buying: realize (avg_entry - buy_price) * closed_qty.
      pnl = (s.avg_entry - price) * closed_qty;
    }
    s.realized += pnl;
    total_realized_ += pnl;
  }

  // Update average entry price.
  if (new_inv == 0) {
    s.avg_entry = 0.0;
  } else if ((old_inv >= 0 && delta > 0) || (old_inv <= 0 && delta < 0)) {
    // Adding to position: VWAP update.
    double total_cost = s.avg_entry * static_cast<double>(std::abs(old_inv)) +
                        price * static_cast<double>(std::abs(delta));
    s.avg_entry = total_cost / static_cast<double>(std::abs(new_inv));
  }
  // If reducing position, avg_entry stays the same.

  s.inventory = new_inv;
  total_turnover_ += static_cast<double>(r.qty);

  // Adverse selection: record this fill and classify any pending fills
  // whose lookback window has expired.
  ++total_fills_;
  pending_fills_.push_back(
      {r.symbol, ts, price, r.side,
       (r.side == Side::Buy) ? static_cast<std::int64_t>(r.qty)
                              : -static_cast<std::int64_t>(r.qty)});
  classify_pending(ts);

  // Update equity and drawdown.
  double equity = net_pnl();
  if (equity > peak_equity_) peak_equity_ = equity;
  double dd = peak_equity_ - equity;
  if (dd > max_drawdown_) max_drawdown_ = dd;

  // Return tracking (per-event return).
  if (return_count_ > 0) {
    double ret = equity - last_equity_;
    sum_returns_ += ret;
    sum_sq_returns_ += ret * ret;
    if (ret < 0) sum_neg_sq_returns_ += ret * ret;
  }
  return_count_++;
  last_equity_ = equity;
}

void Analytics::mark_to_market(SymbolID sym, double mid_price) {
  ensure_symbol(sym);
  auto& s = symbols_[static_cast<std::size_t>(sym)];
  s.last_mid = mid_price;

  // Classify pending fills for this symbol: if mid moved against the fill
  // by more than tick_size, it was toxic (adverse selection).
  for (auto it = pending_fills_.begin(); it != pending_fills_.end();) {
    if (it->symbol != sym) {
      ++it;
      continue;
    }
    double mid_move = (it->side == Side::Buy)
                          ? (mid_price - it->fill_price)
                          : (it->fill_price - mid_price);
    if (mid_move < -s.tick_size) {
      // Mid moved against us: toxic fill.
      double cost = std::abs(mid_move) * static_cast<double>(std::abs(it->qty));
      toxic_cost_ += cost;
      ++toxic_fills_;
      it = pending_fills_.erase(it);
    } else {
      ++it;
    }
  }

  // Recompute unrealized PnL.
  total_unrealized_ = 0.0;
  for (const auto& ss : symbols_) {
    if (ss.inventory != 0 && ss.last_mid > 0) {
      total_unrealized_ +=
          (ss.last_mid - ss.avg_entry) * static_cast<double>(ss.inventory);
    }
  }
}

void Analytics::classify_pending(Timestamp now) {
  // Drop fills whose lookback window has expired without a mid update —
  // they can't be classified, so we treat them as non-toxic.
  while (!pending_fills_.empty() &&
         (now - pending_fills_.front().fill_ts) > kToxicLookbackNs) {
    pending_fills_.pop_front();
  }
}

double Analytics::sharpe_ratio() const noexcept {
  if (return_count_ < 2) return 0.0;
  double mean = sum_returns_ / static_cast<double>(return_count_);
  double var = (sum_sq_returns_ / static_cast<double>(return_count_)) - mean * mean;
  if (var <= 0.0) return 0.0;
  return mean / std::sqrt(var) * std::sqrt(252.0);
}

double Analytics::sortino_ratio() const noexcept {
  if (return_count_ < 2) return 0.0;
  double mean = sum_returns_ / static_cast<double>(return_count_);
  double neg_var =
      sum_neg_sq_returns_ / static_cast<double>(return_count_);
  if (neg_var <= 0.0) return 0.0;
  return mean / std::sqrt(neg_var) * std::sqrt(252.0);
}

void Analytics::print(std::FILE* out) const {
  std::fprintf(out, "=== Analytics ===\n");
  std::fprintf(out, "  Realized PnL:   %.4f\n", total_realized_);
  std::fprintf(out, "  Unrealized PnL: %.4f\n", total_unrealized_);
  std::fprintf(out, "  Total Fees:     %.4f\n", total_fees_);
  std::fprintf(out, "  Net PnL:        %.4f\n", net_pnl());
  std::fprintf(out, "  Max Drawdown:   %.4f\n", max_drawdown_);
  std::fprintf(out, "  Sharpe:         %.4f\n", sharpe_ratio());
  std::fprintf(out, "  Sortino:        %.4f\n", sortino_ratio());
  std::fprintf(out, "  Turnover:       %.2f\n", total_turnover_);
  std::fprintf(out, "  Toxic fills:    %llu / %llu (%.1f%%)\n",
               static_cast<unsigned long long>(toxic_fills_),
               static_cast<unsigned long long>(total_fills_),
               toxic_fill_ratio() * 100.0);
  std::fprintf(out, "  Avg toxic cost: %.6f\n", avg_toxic_cost());
}

void Analytics::write_pnl_csv(const std::string& path) const {
  // Simplified: just write a single summary row. A full implementation would
  // track the equity curve over time.
  std::ofstream out(path);
  if (!out) return;
  out << "metric,value\n";
  out << "realized_pnl," << total_realized_ << "\n";
  out << "unrealized_pnl," << total_unrealized_ << "\n";
  out << "fees," << total_fees_ << "\n";
  out << "net_pnl," << net_pnl() << "\n";
  out << "max_drawdown," << max_drawdown_ << "\n";
  out << "sharpe," << sharpe_ratio() << "\n";
  out << "sortino," << sortino_ratio() << "\n";
  out << "turnover," << total_turnover_ << "\n";
}

}  // namespace aster::strategy
