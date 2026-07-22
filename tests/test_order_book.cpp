#include <array>
#include <iostream>
#include <cassert>

#include "lob/matching_engine.hpp"

using namespace lob;
namespace{
    void test_basic_limit_match(){
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> trades;
        std::size_t n = engine.addLimitOrder(1, Side::SELL, 100, 10, trades);

        assert(n==0);
        assert(engine.bestAsk()==100);

        n = engine.addLimitOrder(2, Side::BUY, 100, 4, trades);
        assert(n==1);
        assert(trades[0].maker_id == 1);
        assert(trades[0].taker_id == 2);
        assert(trades[0].quantity == 4);
        assert(engine.bestAsk() == 100);//6 left resting
    }

    void test_price_time_priority(){
        //setup
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> trades;

        //put two orders
        engine.addLimitOrder(1, Side::SELL, 100, 10, trades);
        engine.addLimitOrder(2, Side::SELL, 100, 10, trades);

        std::size_t n = engine.addLimitOrder(3, Side::BUY, 100, 10, trades);
        assert(n==1);
        assert(trades[0].maker_id==1);
        
        //another order with better ask price
        engine.addLimitOrder(3, Side::SELL, 99, 10, trades);
        n = engine.addLimitOrder(4, Side::BUY, 100, 5, trades);
        assert(trades[0].maker_id==3);
    }

    void test_market_order_sweep(){
        //a large market order is able to fill across price levels
        //setup
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> trades;

        engine.addLimitOrder(1, Side::SELL, 100, 10, trades);
        engine.addLimitOrder(2, Side::SELL, 101, 5, trades);

        std::size_t n = engine.addMarketOrder(3, Side::BUY, 12, trades);
        assert(n==2);
        assert(trades[0].maker_id == 1 && trades[0].quantity == 10);
        assert(trades[1].maker_id == 2 && trades[1].quantity == 2);
        assert(engine.bestAsk()==101);
    }

    void test_cancel_and_pool_reuse(){
        //setup 
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> trades;

        engine.addLimitOrder(1, Side::SELL, 100, 10, trades);
        assert(engine.restingOrderCount()==1);

        engine.cancelOrder(1);
        assert(engine.restingOrderCount()==0);
        assert(engine.bestAsk()==kInvalidPrice);
        assert(!engine.cancelOrder(1));//it isn't live, can't cancel
        
        //add another order
        engine.addLimitOrder(2, Side::SELL, 100, 3, trades);
        assert(engine.restingOrderCount()==1);
    }

    void test_best_price_advances_after_level_empties(){
        //setup 
        MatchingEngine engine("TEST", 1000);
        std::array<Trade, 4> trades;

        engine.addLimitOrder(1, Side::SELL, 100, 5 ,trades);
        assert(engine.bestAsk()==100);
        engine.addLimitOrder(2, Side::SELL, 99, 5, trades);
        assert(engine.bestAsk()==99);

        engine.cancelOrder(2);
        assert(engine.bestAsk()==100);
    }
}

int main(){
    test_basic_limit_match();
    test_price_time_priority();
    test_market_order_sweep();
    test_cancel_and_pool_reuse();
    test_best_price_advances_after_level_empties();
    std::cout<<"All tests passed";
    return 0;
}
