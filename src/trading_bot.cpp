#include "lob/trading_bot.hpp"

#include <cmath> //not used
#include <algorithm>  //for clamp

namespace lob{
    namespace{
        constexpr double kReversionRate = 0.05;
        constexpr double kNoiseStd = 3.0;
        constexpr double kSpread = 5.0;

    }//anonymous namespace, only visible inside this .cpp file
    TradingBot::TradingBot(std::uint64_t seed, Price mean_price, Price max_price): 
    rng_(seed),
    noise_(0.0, kNoiseStd), 
    mean_price_(mean_price), 
    max_price_(max_price),
    current_price_(mean_price) //would this need static_cast<double>
    {}

    BotOrder TradingBot::next(){
        //a mean-reversing random walk for simplicity 
        //there are other stochastic models, but this one is simple and suffices for exercising the orderbook
        //next step : replay historical orderbook data
        current_price_ += kReversionRate*(static_cast<double>(mean_price_) - current_price_) + noise_((rng_));
        current_price_ = std::clamp(current_price_, 1.0, static_cast<double>(max_price_-1));

        //to simulate percentages out of 100
        std::uniform_int_distribution<int> action_pick(0, 99);
        int roll = action_pick(rng_);

        //15% are cancel orders
        
        if(roll<15 && !live_orders_.empty()){
            //chose an order to cancel
            //if a filled order is chosen, that will be a no-op call
            std::uniform_int_distribution<std::size_t> pick(0, live_orders_.size()-1);
            std::size_t idx = pick(rng_);
            OrderId id = live_orders_[idx];
            live_orders_[idx] = live_orders_.back();
            live_orders_.pop_back();
            return BotOrder{.action = BotAction::CANCEL, .id = id};
        }

        //buy or sell and quantity
        std::bernoulli_distribution side_coin(0.5);
        Side side = side_coin(rng_)? Side::BUY : Side::SELL;
        std::uniform_int_distribution<Quantity> qty_dist(1, 20);
        Quantity qty = qty_dist(rng_);

        //10% are market orders
        if(roll < 25){
            return BotOrder{.action = BotAction::MARKET, .side = side, .quantity = qty};
        }

        //the rest are limit orders
        //if it is buy, then it tends to be below current price
        std::uniform_real_distribution<double> offset(0.0, kSpread);

        double raw_price = current_price_ + ((side == Side::BUY)? -1.0 : 1.0 )* offset(rng_);
        Price limit_price = static_cast<Price>(std::clamp(raw_price, 1.0, static_cast<double>(max_price_-1)));
        OrderId id = next_id_; next_id_++;
        live_orders_.push_back(id);
        return BotOrder{.action = BotAction::LIMIT, .id = id, .side = side, .price = limit_price, .quantity=qty};
    }


}