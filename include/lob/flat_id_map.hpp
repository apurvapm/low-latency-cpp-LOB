#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "lob/types.hpp"

namespace lob {

    // Preallocated open-addressing map from OrderId -> PoolIndex: linear
    // probing, power-of-two capacity, backward-shift deletion (no
    // tombstones). An empty slot is denoted by value == kInvalidIndex, never
    // by a reserved key, so any OrderId (including 0) is a valid key.
    // All operations allocate nothing after construction.
    class FlatIdMap {
        public:
        explicit FlatIdMap(std::size_t order_capacity)
            : capacity_(std::bit_ceil(std::max<std::size_t>(order_capacity * 2, 2))),
              mask_(capacity_ - 1),
              slots_(std::make_unique<Slot[]>(capacity_))
        {}

        [[nodiscard]] PoolIndex find(OrderId id) const noexcept {
            std::size_t i = home(id);
            while (slots_[i].value != kInvalidIndex) {
                if (slots_[i].key == id) return slots_[i].value;
                i = (i + 1) & mask_;
            }
            return kInvalidIndex;
        }

        // Returns false without modifying anything if id is already present.
        bool insert(OrderId id, PoolIndex idx) noexcept {
            std::size_t i = home(id);
            while (slots_[i].value != kInvalidIndex) {
                if (slots_[i].key == id) return false;
                i = (i + 1) & mask_;
            }
            slots_[i] = Slot{id, idx};
            ++size_;
            return true;
        }

        bool erase(OrderId id) noexcept {
            std::size_t i = home(id);
            while (true) {
                if (slots_[i].value == kInvalidIndex) return false;
                if (slots_[i].key == id) break;
                i = (i + 1) & mask_;
            }
            backwardShift(i);
            --size_;
            return true;
        }

        [[nodiscard]] std::size_t size() const noexcept { return size_; }

        template <typename Fn>
        void forEach(Fn&& fn) const {
            for (std::size_t i = 0; i < capacity_; ++i) {
                if (slots_[i].value != kInvalidIndex) fn(slots_[i].key, slots_[i].value);
            }
        }

        private:
        struct Slot {
            OrderId key = 0;
            PoolIndex value = kInvalidIndex;
        };

        // splitmix64's finalizer, used purely as a bit mixer here.
        static constexpr std::uint64_t mix(std::uint64_t x) noexcept {
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebULL;
            x ^= x >> 31;
            return x;
        }

        [[nodiscard]] std::size_t home(OrderId id) const noexcept {
            return static_cast<std::size_t>(mix(static_cast<std::uint64_t>(id)) & static_cast<std::uint64_t>(mask_));
        }

        // Removes the occupant at `hole` and shifts later entries of its
        // probe chain backward, so later find()s still terminate correctly
        // without needing a tombstone. See Wikipedia "Open addressing" ->
        // Deletion, or Knuth's Algorithm R6.
        void backwardShift(std::size_t hole) noexcept {
            std::size_t j = hole;
            while (true) {
                j = (j + 1) & mask_;
                if (slots_[j].value == kInvalidIndex) break;
                std::size_t k = home(slots_[j].key);
                // Skip (leave slot j alone) if k lies cyclically in (hole, j];
                // otherwise slot j can move back into hole.
                bool k_in_range = (hole <= j) ? (hole < k && k <= j)
                                               : (hole < k || k <= j);
                if (k_in_range) continue;
                slots_[hole] = slots_[j];
                hole = j;
            }
            slots_[hole] = Slot{};
        }

        std::size_t capacity_;
        std::size_t mask_;
        std::unique_ptr<Slot[]> slots_;
        std::size_t size_ = 0;
    };

} // namespace lob
