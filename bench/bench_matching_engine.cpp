#include <array>

#include <benchmark/benchmark.h>

#include "lob/matching_engine.hpp"
#include "lob/trading_bot.hpp"

namespace{
    constexpr lob::Price kMaxPrice =100'000;
    constexpr lob::Price kMeanPrice = 50'000;
    // Pool is kOrderCapacity == 1<<20. At ~75% limits / ~15% cancels this
    // stream rests at most ~393k orders, so the pool cannot exhaust mid-run.
    constexpr std::size_t kStreamLength = 1u << 20;

    // Generated once for the whole process: every repetition replays the same
    // sequence, so run-to-run spread reflects the machine, not the RNG.
    const std::vector<lob::BotOrder>& orderStream(){
        static const std::vector<lob::BotOrder> stream = []{
            lob::TradingBot bot(1, kMeanPrice, kMaxPrice);
            std::vector<lob::BotOrder> v;
            v.reserve(kStreamLength);
            for(std::size_t i = 0; i < kStreamLength; i++) v.push_back(bot.next());
            return v;
        }();
        return stream;
    }
}
static void BM_MatchingEngineOrderFlow(benchmark::State& state){
    //setup
    lob::MatchingEngine engine("BENCH", kMaxPrice);
    lob::TradingBot bot(1, kMeanPrice, kMaxPrice);
    std::array<lob::Trade, 64> trades;

    for(auto _ : state)//runs until the value becomes statistically stable, but we later fix the iterations to remain within the bounds of the orderbooks capacity
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
BENCHMARK(BM_MatchingEngineOrderFlow)->Iterations(kStreamLength)->Unit(benchmark::kNanosecond);

static void BM_MatchingEngineOrderFlow_no_bot(benchmark::State& state){
    const std::vector<lob::BotOrder>& stream = orderStream();

    lob::MatchingEngine engine("BENCH", kMaxPrice);
    std::array<lob::Trade, 64> trades;
    std::size_t cursor = 0;

    if(static_cast<std::size_t>(state.max_iterations) > stream.size()){
        state.SkipWithError("order stream shorter than iteration count");
        return;
    }

    for(auto _ : state)
    {
        const lob::BotOrder& order = stream[cursor++];
        switch(order.action){
            case lob::BotAction::MARKET :
            {
                benchmark::DoNotOptimize(
                    engine.addMarketOrder(order.id, order.side, order.quantity, trades));
                break;
            }
            case lob::BotAction::LIMIT:
            {
                benchmark::DoNotOptimize(
                    engine.addLimitOrder(order.id, order.side, order.price, order.quantity, trades));
                break;
            }
            case lob::BotAction::CANCEL:
            {
                benchmark::DoNotOptimize(engine.cancelOrder(order.id));
                break;
            }
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MatchingEngineOrderFlow_no_bot)->Iterations(kStreamLength)->Unit(benchmark::kNanosecond);

//future scope -- use historical orderbook data so that latency of bot is not included

static void BM_InsertCancelRoundTrip(benchmark::State& state)
{
    //set-up
    lob::MatchingEngine engine("BENCH", kMaxPrice);
    ///no-bot here(involves the rng and distributions delays), no matching
    //just measure the bookkeeping
    std::array<lob::Trade, 4> trades;
    lob::OrderId id = 2;
    engine.addLimitOrder(1, lob::Side::BUY, 100, 10, trades); //keeps the level non-empty


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