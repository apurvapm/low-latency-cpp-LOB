#include <array>

#include <benchmark/benchmark.h>

#include "lob/matching_engine.hpp"
#include "lob/trading_bot.hpp"

namespace{
    constexpr lob::Price kMaxPrice =100'000;
    constexpr lob::Price kMeanPrice = 50'000;
}
static void BM_MatchingEngineOrderFlow(benchmark::State& state){
    //setup
    lob::MatchingEngine engine("BENCH", kMaxPrice);
    lob::TradingBot bot(1, kMeanPrice, kMaxPrice);
    std::array<lob::Trade, 64> trades;

    for(auto _ : state)//runs until the value becomes statistically stable
    {
        lob::BotOrder order = bot.next();
        switch(order.action){
            case lob::BotAction::MARKET :
            {
                engine.addMarketOrder(order.id, order.side, order.quantity, trades);
                break;
            }
            case lob::BotAction::LIMIT:
            {
                engine.addLimitOrder(order.id, order.side, order.price, order.quantity, trades);
                break;
            }
            case lob::BotAction::CANCEL:
            {
                engine.cancelOrder(order.id);
                break;
            }
        }
    }
    state.SetItemsProcessed(state.iterations()); 
}
BENCHMARK(BM_MatchingEngineOrderFlow)->Unit(benchmark::kNanosecond);


//future scope -- use historical orderbook data so that latency of bot is not included
static void BM_InsertCancelRoundTrip(benchmark::State& state)
{
    //set-up
    lob::MatchingEngine engine("BENCH", kMaxPrice);
    ///no-bot here(involves the rng and distributions delays), no matching
    //just measure the bookkeeping
    std::array<lob::Trade, 4> trades;
    lob::OrderId id = 1;

    for(auto _ : state){
        //add a BUY LIMIT order
        lob::OrderId cur_id = id++;
        engine.addLimitOrder(cur_id, lob::Side::BUY, 100, 10, trades);
        benchmark::DoNotOptimize(engine.cancelOrder(cur_id));
    }
    state.SetItemsProcessed(state.iterations()); // to report items/sec as well, along with the default ns/iteration
}

BENCHMARK(BM_InsertCancelRoundTrip)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();