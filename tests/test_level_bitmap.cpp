// Deterministic checks at the exact word/group boundaries, plus a randomised
// comparison against a brute-force linear scan over a bounded tick window
// (kept small so the O(n) reference stays fast; it still spans an L0 word
// boundary at 63/64 and the L1 group boundary at 4095/4096).

#include <array>
#include <cstddef>
#include <iostream>
#include <random>
#include <vector>

#include "check.hpp"
#include "lob/level_bitmap.hpp"
#include "lob/types.hpp"

using namespace lob;

namespace {
    Price bruteNextAtOrAbove(const std::vector<bool>& ref, Price p) {
        if (p < 0) p = 0;
        for (Price t = p; t < static_cast<Price>(ref.size()); ++t) {
            if (ref[static_cast<std::size_t>(t)]) return t;
        }
        return kInvalidPrice;
    }
    Price brutePrevAtOrBelow(const std::vector<bool>& ref, Price p) {
        if (p < 0) return kInvalidPrice;
        if (static_cast<std::size_t>(p) >= ref.size()) p = static_cast<Price>(ref.size() - 1);
        for (Price t = p; t >= 0; --t) {
            if (ref[static_cast<std::size_t>(t)]) return t;
        }
        return kInvalidPrice;
    }

    void testExactBoundaries() {
        LevelBitmap bitmap;
        // 0 = range start; 63/64 = L0 word boundary; 4095/4096 = L1 group
        // boundary (64 L0 words * 64 bits); kMaxPriceTicks-1 = range end.
        bitmap.set(0);
        CHECK(bitmap.nextSetAtOrAbove(0) == 0);
        CHECK(bitmap.prevSetAtOrBelow(0) == 0);
        CHECK(bitmap.prevSetAtOrBelow(-1) == kInvalidPrice);

        bitmap.set(63);
        CHECK(bitmap.nextSetAtOrAbove(1) == 63);
        bitmap.clear(0);
        CHECK(bitmap.prevSetAtOrBelow(63) == 63);
        CHECK(bitmap.nextSetAtOrAbove(64) == kInvalidPrice);

        bitmap.set(64);
        CHECK(bitmap.nextSetAtOrAbove(64) == 64);
        CHECK(bitmap.nextSetAtOrAbove(1) == 63); // still finds 63 before 64
        bitmap.clear(63);
        CHECK(bitmap.prevSetAtOrBelow(64) == 64);
        CHECK(bitmap.prevSetAtOrBelow(63) == kInvalidPrice);

        bitmap.clear(64);
        bitmap.set(4095);
        CHECK(bitmap.nextSetAtOrAbove(0) == 4095);
        CHECK(bitmap.prevSetAtOrBelow(4095) == 4095);
        CHECK(bitmap.nextSetAtOrAbove(4096) == kInvalidPrice);

        bitmap.set(4096);
        CHECK(bitmap.nextSetAtOrAbove(4096) == 4096);
        CHECK(bitmap.nextSetAtOrAbove(0) == 4095);
        bitmap.clear(4095);
        CHECK(bitmap.prevSetAtOrBelow(4096) == 4096);
        CHECK(bitmap.prevSetAtOrBelow(4095) == kInvalidPrice);

        bitmap.clear(4096);
        constexpr Price kLast = static_cast<Price>(kMaxPriceTicks - 1);
        bitmap.set(kLast);
        CHECK(bitmap.nextSetAtOrAbove(0) == kLast);
        CHECK(bitmap.prevSetAtOrBelow(kLast) == kLast);
        CHECK(bitmap.nextSetAtOrAbove(kLast + 1) == kInvalidPrice);
        CHECK(bitmap.nextSetAtOrAbove(static_cast<Price>(kMaxPriceTicks)) == kInvalidPrice);
        bitmap.clear(kLast);
        CHECK(bitmap.nextSetAtOrAbove(0) == kInvalidPrice);
        CHECK(bitmap.prevSetAtOrBelow(kLast) == kInvalidPrice);
    }

    void testRandomisedAgainstBruteForce() {
        constexpr std::size_t kWindow = 8192; // spans the 63/64 and 4095/4096 boundaries
        constexpr int kOps = 20'000;
        constexpr int kQueriesPerOp = 2;

        LevelBitmap bitmap;
        std::vector<bool> ref(kWindow, false);

        std::mt19937_64 rng(777);
        std::uniform_int_distribution<Price> tick_pick(0, static_cast<Price>(kWindow - 1));
        std::bernoulli_distribution coin(0.5);
        const std::array<Price, 6> boundary_ticks = {0, 63, 64, 4095, 4096, static_cast<Price>(kWindow - 1)};

        for (int step = 0; step < kOps; ++step) {
            Price p = (step % 5 == 0) ? boundary_ticks[static_cast<std::size_t>(rng()) % boundary_ticks.size()]
                                       : tick_pick(rng);
            if (coin(rng)) {
                bitmap.set(p);
                ref[static_cast<std::size_t>(p)] = true;
            } else {
                bitmap.clear(p);
                ref[static_cast<std::size_t>(p)] = false;
            }

            for (int q = 0; q < kQueriesPerOp; ++q) {
                Price query = (q == 0) ? boundary_ticks[static_cast<std::size_t>(rng()) % boundary_ticks.size()]
                                        : tick_pick(rng);
                CHECK(bitmap.nextSetAtOrAbove(query) == bruteNextAtOrAbove(ref, query));
                CHECK(bitmap.prevSetAtOrBelow(query) == brutePrevAtOrBelow(ref, query));
            }
        }
    }
}

int main() {
    testExactBoundaries();
    testRandomisedAgainstBruteForce();

    if (lob::test::failureCount() != 0) {
        std::cout << lob::test::failureCount() << " check(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed";
    return 0;
}
