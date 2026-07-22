#pragma once

#include "lob/types.hpp"

namespace lob{
    struct alignas(32) Order{ // align to 32 bytes, making it cache friendly, cacheline size is 64 bytes
        OrderId id=0; //why not make an invlaid OrderId
        // OrderType type; //not needed as the order types LIMIT and MARKET are handled by the engine at runtime
        Side side = Side::BUY; 
        Price price=kInvalidPrice;
        Quantity quantity=0;
        PoolIndex prev=kInvalidIndex ; //for the FIFO time-price priority queue
        PoolIndex next=kInvalidIndex ;
    };
} //namespace lob