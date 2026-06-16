#include <iostream>
#include <cassert>
#include "aster/matching_engine.hpp"
#include "aster/types.hpp"

using namespace aster;

int main() {
    MatchingEngine eng(1000);
    std::vector<TradeEvent> trades;
    eng.on_trade([&](const TradeEvent& t){ trades.push_back(t); });

    // 1. Simple limit match
    eng.process({0,0,1,"T",Side::Buy,OrderType::Limit,100,50,EventType::AddOrder});
    eng.process({0,1,2,"T",Side::Sell,OrderType::Limit,100,30,EventType::AddOrder});
    assert(trades.size() == 1);
    assert(trades[0].quantity == 30);
    eng.clear(); trades.clear();

    // 2. Price-time priority
    eng.process({0,0,1,"T",Side::Buy,OrderType::Limit,101,10,EventType::AddOrder});
    eng.process({0,1,2,"T",Side::Buy,OrderType::Limit,102,5,EventType::AddOrder});
    eng.process({0,2,3,"T",Side::Sell,OrderType::Limit,101,12,EventType::AddOrder});
    assert(trades.size() == 2);
    assert(trades[0].price == 102 && trades[0].quantity == 5);
    assert(trades[1].price == 101 && trades[1].quantity == 7);
    eng.clear(); trades.clear();

    // 3. Market buy
    eng.process({0,0,1,"T",Side::Sell,OrderType::Limit,100,10,EventType::AddOrder});
    eng.process({0,1,2,"T",Side::Buy,OrderType::Market,0,15,EventType::AddOrder});
    assert(trades.size() == 1);
    assert(trades[0].quantity == 10);
    eng.clear(); trades.clear();

    // 4. Market sell
    eng.process({0,0,1,"T",Side::Buy,OrderType::Limit,100,20,EventType::AddOrder});
    eng.process({0,1,2,"T",Side::Sell,OrderType::Market,0,10,EventType::AddOrder});
    assert(trades.size() == 1);
    assert(trades[0].quantity == 10);
    eng.clear(); trades.clear();

    // 5. OrderReduce
    eng.process({0,0,1,"T",Side::Buy,OrderType::Limit,100,50,EventType::AddOrder});
    eng.process({0,1,1,"T",Side::Buy,OrderType::Limit,0,30,EventType::OrderReduce});
    eng.process({0,2,2,"T",Side::Sell,OrderType::Limit,100,40,EventType::AddOrder});
    assert(trades.size() == 1);
    assert(trades[0].quantity == 30);
    eng.clear(); trades.clear();

    // 6. Cancel
    eng.process({0,0,1,"T",Side::Buy,OrderType::Limit,100,10,EventType::AddOrder});
    eng.process({0,1,1,"T",Side::Buy,OrderType::Limit,0,0,EventType::OrderCancel});
    eng.process({0,2,2,"T",Side::Sell,OrderType::Limit,100,10,EventType::AddOrder});
    assert(trades.empty());
    eng.clear(); trades.clear();

    // 7. Deterministic replay
    auto run_seq = [](std::vector<OrderEvent> seq) {
        MatchingEngine e(1000);
        std::vector<TradeEvent> tr;
        e.on_trade([&](const TradeEvent& t){tr.push_back(t);});
        for (auto& ev : seq) e.process(ev);
        return tr;
    };
    std::vector<OrderEvent> seq = {
        {0,0,1,"A",Side::Buy,OrderType::Limit,100,5,EventType::AddOrder},
        {0,1,2,"A",Side::Sell,OrderType::Limit,100,5,EventType::AddOrder}
    };
    auto tr1 = run_seq(seq);
    auto tr2 = run_seq(seq);
    assert(tr1.size() == tr2.size() && tr1[0].price == tr2[0].price && tr1[0].quantity == tr2[0].quantity);

    std::cout << "All extended tests passed.\n";
    return 0;
}
