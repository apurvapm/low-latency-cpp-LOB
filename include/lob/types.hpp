#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace lob{

    using Price = std::int32_t;
    using OrderId = std::uint64_t; //made 64 bit because compared to other types here , the range og orders can be very high
    using Quantity = std::uint32_t;
    using PoolIndex = std::uint32_t;

    inline constexpr PoolIndex kInvalidIndex = std::numeric_limits<PoolIndex>::max();
    inline constexpr Price kInvalidPrice = -1;
    inline constexpr std::size_t kMaxPriceTicks = 100'000; // upto $999.99 at a $0.01 per tick

    enum class OrderType: std::uint8_t {
        LIMIT, MARKET
    };
    enum class Side: std::uint8_t {
        BUY, SELL
    };

    struct Trade{
        //all the following will be asigned
        OrderId maker_id, taker_id; 
        Price price;
        Quantity quantity;
    };
} //namespace lob