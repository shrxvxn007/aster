#include "aster/matching_engine.hpp"

namespace aster {

MatchingEngine::MatchingEngine(size_t order_pool_capacity)
    : book_(order_pool_capacity) {}

std::vector<TradeEvent> MatchingEngine::process(const OrderEvent& event) {
    auto trades = book_.process(event);

    for (const auto& t : trades)
        if (trade_cb_) trade_cb_(t);

    if (book_update_cb_) {
        std::vector<BookUpdate> updates;
        book_.snapshot(event.symbol, updates, event.timestamp);
        for (const auto& u : updates)
            book_update_cb_(u);
    }

    return trades;
}

void MatchingEngine::on_trade(TradeCallback cb) { trade_cb_ = std::move(cb); }
void MatchingEngine::on_book_update(BookUpdateCallback cb) { book_update_cb_ = std::move(cb); }
void MatchingEngine::snapshot(const Symbol& sym, std::vector<BookUpdate>& updates, Timestamp now) const {
    book_.snapshot(sym, updates, now);
}
void MatchingEngine::clear() { book_.clear(); }

} // namespace aster
