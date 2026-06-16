#pragma once

#include "aster/types.hpp"
#include "aster/order_book.hpp"
#include <functional>

namespace aster {

class MatchingEngine {
public:
    explicit MatchingEngine(size_t order_pool_capacity = 1'000'000);

    std::vector<TradeEvent> process(const OrderEvent& event);

    using TradeCallback = std::function<void(const TradeEvent&)>;
    using BookUpdateCallback = std::function<void(const BookUpdate&)>;

    void on_trade(TradeCallback cb);
    void on_book_update(BookUpdateCallback cb);
    void snapshot(const Symbol& sym, std::vector<BookUpdate>& updates, Timestamp now) const;
    void clear();

private:
    OrderBook book_;
    TradeCallback trade_cb_;
    BookUpdateCallback book_update_cb_;
};

} // namespace aster
