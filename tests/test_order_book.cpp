#include <array>
#include <iostream>
#include <optional>
#include <string>

#include "check.hpp"
#include "lob/matching_engine.hpp"

using namespace lob;
namespace{
    void test_basic_limit_match(){
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> trades;
        std::size_t n = engine.addLimitOrder(1, Side::SELL, 100, 10, trades);

        CHECK(n==0);
        CHECK(engine.bestAsk()==100);

        n = engine.addLimitOrder(2, Side::BUY, 100, 4, trades);
        CHECK(n==1);
        CHECK(trades[0].maker_id == 1);
        CHECK(trades[0].taker_id == 2);
        CHECK(trades[0].quantity == 4);
        CHECK(engine.bestAsk() == 100);//6 left resting
    }

    void test_price_time_priority(){
        //setup
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> trades;

        //put two orders
        engine.addLimitOrder(1, Side::SELL, 100, 10, trades);
        engine.addLimitOrder(2, Side::SELL, 100, 10, trades);

        std::size_t n = engine.addLimitOrder(3, Side::BUY, 100, 10, trades);
        CHECK(n==1);
        CHECK(trades[0].maker_id==1);

        //another order with better ask price
        engine.addLimitOrder(3, Side::SELL, 99, 10, trades);
        n = engine.addLimitOrder(4, Side::BUY, 100, 5, trades);
        CHECK(trades[0].maker_id==3);
    }

    void test_market_order_sweep(){
        //a large market order is able to fill across price levels
        //setup
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> trades;

        engine.addLimitOrder(1, Side::SELL, 100, 10, trades);
        engine.addLimitOrder(2, Side::SELL, 101, 5, trades);

        std::size_t n = engine.addMarketOrder(3, Side::BUY, 12, trades);
        CHECK(n==2);
        CHECK(trades[0].maker_id == 1 && trades[0].quantity == 10);
        CHECK(trades[1].maker_id == 2 && trades[1].quantity == 2);
        CHECK(engine.bestAsk()==101);
    }

    void test_cancel_and_pool_reuse(){
        //setup 
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> trades;

        engine.addLimitOrder(1, Side::SELL, 100, 10, trades);
        CHECK(engine.restingOrderCount()==1);

        engine.cancelOrder(1);
        CHECK(engine.restingOrderCount()==0);
        CHECK(engine.bestAsk()==kInvalidPrice);
        CHECK(!engine.cancelOrder(1));//it isn't live, can't cancel

        //add another order
        engine.addLimitOrder(2, Side::SELL, 100, 3, trades);
        CHECK(engine.restingOrderCount()==1);
    }

    void test_best_price_advances_after_level_empties(){
        //setup 
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> trades;

        engine.addLimitOrder(1, Side::SELL, 100, 5 ,trades);
        CHECK(engine.bestAsk()==100);
        engine.addLimitOrder(2, Side::SELL, 99, 5, trades);
        CHECK(engine.bestAsk()==99);

        engine.cancelOrder(2);
        CHECK(engine.bestAsk()==100);
    }

    void test_rejects_invalid_orders(){
        //P3: an out-of-range price or zero quantity is rejected, book untouched
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> trades;

        CHECK(engine.addLimitOrder(1, Side::SELL, -1, 10, trades) == 0);
        CHECK(engine.addLimitOrder(2, Side::SELL, 1000, 10, trades) == 0); // == max_price_
        CHECK(engine.addLimitOrder(3, Side::SELL, 100, 0, trades) == 0);
        CHECK(engine.addMarketOrder(4, Side::BUY, 0, trades) == 0);

        CHECK(engine.rejectedCount() == 4);
        CHECK(engine.restingOrderCount() == 0);
        CHECK(engine.bestAsk() == kInvalidPrice);
    }

    void test_pool_exhaustion_drops_order(){
        //P4: a full pool drops the resting remainder instead of losing it silently
        MatchingEngine engine("TEST", 1000, /*order_capacity*/4);
        std::array<Trade, 4> trades;

        for(OrderId id = 1; id <= 4; ++id){
            engine.addLimitOrder(id, Side::SELL, static_cast<Price>(100 + id), 1, trades);
        }
        CHECK(engine.restingOrderCount() == 4);
        CHECK(engine.droppedCount() == 0);

        CHECK(engine.addLimitOrder(5, Side::SELL, 200, 1, trades) == 0);
        CHECK(engine.droppedCount() == 1);
        CHECK(engine.restingOrderCount() == 4); // book unaffected
    }

    void test_truncated_trades_reported(){
        //P5: fills beyond the trades span still execute but are counted, not lost
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> setup_trades;
        engine.addLimitOrder(1, Side::SELL, 100, 5, setup_trades);
        engine.addLimitOrder(2, Side::SELL, 100, 5, setup_trades);
        engine.addLimitOrder(3, Side::SELL, 100, 5, setup_trades);

        std::array<Trade, 1> trades; // room for only 1 of the 3 fills
        std::size_t n = engine.addLimitOrder(4, Side::BUY, 100, 15, trades);

        CHECK(n == 1);
        CHECK(engine.truncatedTradeCount() == 2);
        CHECK(engine.bestAsk() == kInvalidPrice); // all 3 asks filled regardless
        CHECK(engine.restingOrderCount() == 0);
    }

    void test_symbol_survives_source_destruction(){
        //P7: the symbol is copied into the engine, not referenced
        std::optional<MatchingEngine> engine;
        {
            std::string temp = "TEMP";
            engine.emplace(temp, 1000);
        } // temp destroyed here
        CHECK(engine->symbol() == "TEMP");
    }
}

int main(){
    test_basic_limit_match();
    test_price_time_priority();
    test_market_order_sweep();
    test_cancel_and_pool_reuse();
    test_best_price_advances_after_level_empties();
    test_rejects_invalid_orders();
    test_pool_exhaustion_drops_order();
    test_truncated_trades_reported();
    test_symbol_survives_source_destruction();

    if (lob::test::failureCount() != 0) {
        std::cout << lob::test::failureCount() << " check(s) failed\n";
        return 1;
    }
    std::cout<<"All tests passed";
    return 0;
}
