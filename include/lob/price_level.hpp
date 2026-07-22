#pragma once
#include "lob/types.hpp"

namespace lob{
    struct PriceLevel{
        PoolIndex head = kInvalidIndex; 
        PoolIndex tail = kInvalidIndex;
        Quantity total_quantity=0; //total quantity that can be displaced in the cli
        // Price price = kInvalidPrice;
        std::uint32_t order_count=0;

        [[nodiscard]] bool empty() const noexcept{ return order_count==0;} //could use total_qty
        //do we allow orders of 0 qty?
    };
}


