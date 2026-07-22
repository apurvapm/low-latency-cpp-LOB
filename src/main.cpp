#include <array>
#include <iostream>

#include "lob/trading_bot.hpp"
#include "lob/matching_engine.hpp"

int main(){
    using namespace lob;

    constexpr Price kMaxPrice = 100'000; // a $100K ? 
    constexpr Price kMeanPrice = 50'000;
    constexpr int kIterations = 200'000;

    MatchingEngine engine("PICKACHU", kMaxPrice);
    TradingBot bot(42, kMeanPrice, kMaxPrice);

    std::array<Trade, 64> trade_buf;
    std::uint64_t total_trades =0;
    for(int i=0; i<kIterations; i++)
    {
        BotOrder order = bot.next();
        switch(order.action){
            case BotAction::LIMIT:
            {
                total_trades += engine.addLimitOrder(order.id, order.side, order.price, order.quantity, trade_buf);
                break;
            }
            case BotAction::MARKET:
            {
                total_trades += engine.addMarketOrder(order.id/*this wil be 0*/,order.side, order.quantity, trade_buf);
                break;
            }
            case BotAction::CANCEL:
            {
                engine.cancelOrder(order.id);
                //returns bool
                //may give a false when the order was already filled
                //false->was a no-op call
                break;
            }
        }
    }
    std::cout<<"Processed "<<kIterations<<" orders from the trading bot"<<'\n';
    std::cout<<"Trades executed: "<<total_trades<<'\n';

    return 0;
}