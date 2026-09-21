#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "lob/types.hpp"

namespace lob {

    // Three-level bitmap over price ticks [0, kMaxPriceTicks): one bit per
    // tick (L0), one bit per non-empty 64-tick L0 word (L1), one bit per
    // non-empty 64-word L1 group (L2, a single word since kL1Words <= 64).
    // Every operation touches a handful of words regardless of how far the
    // next/prev set bit is -- this is what makes advanceBestBid/Ask O(1)
    // instead of O(gap)
    class LevelBitmap {
        public:
        static constexpr std::size_t kL0Words = (kMaxPriceTicks + 63) / 64; // 1563
        static constexpr std::size_t kL1Words = (kL0Words + 63) / 64;       // 25
        static_assert(kL1Words <= 64, "L2 must fit in a single word");

        LevelBitmap() noexcept = default;

        void set(Price p) noexcept {
            std::size_t l0 = wordOf(p);
            bool wasZero = l0_[l0] == 0;
            l0_[l0] |= bitOf(p);
            if (wasZero) {
                std::size_t l1 = l0 / 64;
                bool l1WasZero = l1_[l1] == 0;
                l1_[l1] |= (std::uint64_t{1} << (l0 % 64));
                if (l1WasZero) l2_ |= (std::uint64_t{1} << l1);
            }
        }

        void clear(Price p) noexcept {
            std::size_t l0 = wordOf(p);
            l0_[l0] &= ~bitOf(p);
            if (l0_[l0] == 0) {
                std::size_t l1 = l0 / 64;
                l1_[l1] &= ~(std::uint64_t{1} << (l0 % 64));
                if (l1_[l1] == 0) l2_ &= ~(std::uint64_t{1} << l1);
            }
        }

        // Smallest set tick >= p, or kInvalidPrice if none.
        [[nodiscard]] Price nextSetAtOrAbove(Price p) const noexcept {
            if (p < 0) p = 0;
            if (static_cast<std::size_t>(p) >= kMaxPriceTicks) return kInvalidPrice;

            std::size_t l0 = static_cast<std::size_t>(p) / 64;
            unsigned bit = static_cast<unsigned>(p % 64);

            std::uint64_t w = l0_[l0] & (bit == 0 ? ~std::uint64_t{0} : (~std::uint64_t{0} << bit));
            if (w != 0) return static_cast<Price>(l0 * 64 + static_cast<std::size_t>(std::countr_zero(w)));

            std::size_t l1 = l0 / 64;
            unsigned l0Bit = static_cast<unsigned>(l0 % 64);
            std::uint64_t l1w = l1_[l1] & (l0Bit == 63 ? std::uint64_t{0} : (~std::uint64_t{0} << (l0Bit + 1)));
            if (l1w != 0) {
                std::size_t word = l1 * 64 + static_cast<std::size_t>(std::countr_zero(l1w));
                return static_cast<Price>(word * 64 + static_cast<std::size_t>(std::countr_zero(l0_[word])));
            }

            std::uint64_t l2w = (l1 == 63) ? std::uint64_t{0} : (l2_ & (~std::uint64_t{0} << (l1 + 1)));
            if (l2w == 0) return kInvalidPrice;
            std::size_t nextL1 = static_cast<std::size_t>(std::countr_zero(l2w));
            std::size_t word = nextL1 * 64 + static_cast<std::size_t>(std::countr_zero(l1_[nextL1]));
            return static_cast<Price>(word * 64 + static_cast<std::size_t>(std::countr_zero(l0_[word])));
        }

        // Largest set tick <= p, or kInvalidPrice if none (including p < 0).
        [[nodiscard]] Price prevSetAtOrBelow(Price p) const noexcept {
            if (p < 0) return kInvalidPrice;
            if (static_cast<std::size_t>(p) >= kMaxPriceTicks) p = static_cast<Price>(kMaxPriceTicks - 1);

            std::size_t l0 = static_cast<std::size_t>(p) / 64;
            unsigned bit = static_cast<unsigned>(p % 64);

            std::uint64_t mask = (bit == 63) ? ~std::uint64_t{0} : ((std::uint64_t{1} << (bit + 1)) - 1);
            std::uint64_t w = l0_[l0] & mask;
            if (w != 0) return highestSetPrice(l0, w);

            std::size_t l1 = l0 / 64;
            unsigned l0Bit = static_cast<unsigned>(l0 % 64);
            std::uint64_t l1mask = (l0Bit == 0) ? std::uint64_t{0} : ((std::uint64_t{1} << l0Bit) - 1);
            std::uint64_t l1w = l1_[l1] & l1mask;
            if (l1w != 0) {
                std::size_t word = l1 * 64 + highestSetBit(l1w);
                return highestSetPrice(word, l0_[word]);
            }

            std::uint64_t l2mask = (l1 == 0) ? std::uint64_t{0} : ((std::uint64_t{1} << l1) - 1);
            std::uint64_t l2w = l2_ & l2mask;
            if (l2w == 0) return kInvalidPrice;
            std::size_t prevL1 = highestSetBit(l2w);
            std::size_t word = prevL1 * 64 + highestSetBit(l1_[prevL1]);
            return highestSetPrice(word, l0_[word]);
        }

        private:
        static std::size_t wordOf(Price p) noexcept { return static_cast<std::size_t>(p) / 64; }
        static std::uint64_t bitOf(Price p) noexcept { return std::uint64_t{1} << (static_cast<std::size_t>(p) % 64); }
        static std::size_t highestSetBit(std::uint64_t w) noexcept {
            return static_cast<std::size_t>(63 - std::countl_zero(w));
        }
        static Price highestSetPrice(std::size_t word, std::uint64_t w) noexcept {
            return static_cast<Price>(word * 64 + highestSetBit(w));
        }

        std::array<std::uint64_t, kL0Words> l0_{};
        std::array<std::uint64_t, kL1Words> l1_{};
        std::uint64_t l2_ = 0;
    };

} // namespace lob
