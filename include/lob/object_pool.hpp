#pragma once

#include <cstddef>
#include <concepts>
#include <memory>

#include "lob/types.hpp"

namespace lob{

    // Runtime-sized free-list allocator backed by one heap block, allocated
    // once in the constructor (P6): a fixed 2^20-capacity array cost ~36 MB
    // per engine regardless of how many orders a symbol actually needed.
    template<typename T>
    requires std::default_initializable<T>
    class ObjectPool{
        public:

        explicit ObjectPool(std::size_t capacity)
        : capacity_(capacity),
          storage_(std::make_unique<T[]>(capacity)),
          free_next_(std::make_unique<PoolIndex[]>(capacity))
        {
            for(std::size_t i = 0; (i+1) < capacity_; i++){
                free_next_[i] = static_cast<PoolIndex>(i+1);
            }
            if(capacity_ > 0) free_next_[capacity_-1] = kInvalidIndex;
        }

        [[nodiscard]]PoolIndex acquire() noexcept{
            if(free_head_ == kInvalidIndex)return kInvalidIndex;

            PoolIndex idx = free_head_;
            free_head_ = free_next_[idx];
            return idx;
        }
        void release(PoolIndex idx) noexcept{
            if(idx == kInvalidIndex) return;

            free_next_[idx] = free_head_;
            free_head_ = idx;
        }
        [[nodiscard]]T& operator[](PoolIndex idx) noexcept{return storage_[idx];}
        [[nodiscard]]const T& operator[](PoolIndex idx) const noexcept{return storage_[idx];}
        [[nodiscard]]std::size_t capacity() const noexcept{ return capacity_;}

        private:
        std::size_t capacity_;
        std::unique_ptr<T[]> storage_;
        std::unique_ptr<PoolIndex[]> free_next_;
        PoolIndex free_head_ = 0;
    };
}
