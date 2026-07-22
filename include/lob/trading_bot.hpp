#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "lob/types.hpp"

namespace lob{
    enum class BotAction : std::uint8_t{LIMIT, MARKET, CANCEL};

    struct BotOrder{
        BotAction action = BotAction::LIMIT;
        OrderId id=0;
        Side side = Side::BUY;
        Price price = 0; //could use kInvalid
        Quantity quantity=0;
    };

    class TradingBot{
        public:
            TradingBot(std::uint64_t seed, Price mean_price, Price max_price);

            BotOrder next();
         
        private:
        
            std::mt19937_64 rng_;
            std::normal_distribution<double> noise_;
            Price mean_price_;
            Price max_price_;
            double current_price_;//why is this double and max and mean price are Price which is a uint32_t
            OrderId next_id_ =1;
            std::vector<OrderId> live_orders_;
            //this grows on the 65%-probability limit orders branch and shrinks on 15%-probability cancel branch
            //doesn't get pruned when resting orders get filled
            //those stale ids live in live_orders until randomly chosen leading to a no-op call

    };
}