#pragma once

#include <array>
#include <cstddef>
#include <concepts>

#include "lob/types.hpp"

namespace lob{

//every specialization ObjectPool<Order, 1000>, ObjectPool<2000> is a different type 
    template<typename T, size_t maxCapacity>
    requires std::default_initializable<T>
    class ObjectPool{
        public:

        ObjectPool() noexcept{
            for(size_t i =0; (i+1) < maxCapacity; i++){
                free_next[i] = (i+1);

            }
            free_next[maxCapacity-1] = kInvalidIndex;
        }
        [[nodiscard]]PoolIndex acquire() noexcept{
            if(free_head == kInvalidIndex)return kInvalidIndex;

            PoolIndex idx = free_head;
            free_head = free_next[idx];
            return idx;
        }
        void release(PoolIndex idx) noexcept{
            if(idx == kInvalidIndex) return;

            free_next[idx] = free_head;
            free_head = idx;
        }
        //use nodiscard on all of these
        [[nodiscard]]T& operator[](PoolIndex idx) noexcept{return storage_[idx];}
        [[nodiscard]]const T& operator[](PoolIndex idx) const noexcept{return storage_[idx];}
        [[nodiscard]]static constexpr std::size_t capacity() noexcept{ return maxCapacity;}

        private:
        std::array<T, maxCapacity> storage_;
        std::array<PoolIndex, maxCapacity> free_next;
        PoolIndex free_head =0;
    };
}