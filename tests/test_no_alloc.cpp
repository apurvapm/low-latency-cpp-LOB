// Replaces the global allocator with a counting version, gated by a runtime
// flag so setup (stream generation, engine construction) isn't counted, then
// replays the benchmark's mixed stream and CHECKs zero heap allocations.
//
// Not built under a sanitizer: ASan/UBSan/TSan install their own allocator,
// so overriding operator new/delete here would fight it (see CMakeLists.txt).

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

#include "check.hpp"
#include "lob/matching_engine.hpp"
#include "lob/trading_bot.hpp"

namespace {
    std::atomic<bool> g_counting{false};
    std::atomic<std::size_t> g_alloc_count{0};
}

void* operator new(std::size_t sz) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(sz ? sz : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t sz) {
    return ::operator new(sz);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
    constexpr lob::Price kMaxPrice = 100'000;
    constexpr lob::Price kMeanPrice = 50'000;
    // Same stream shape as bench_matching_engine.cpp: pinned below the order
    // pool's 2^20 capacity so it can't exhaust mid-run.
    constexpr std::size_t kStreamLength = 1u << 20;
}

int main() {
    lob::TradingBot bot(1, kMeanPrice, kMaxPrice);
    std::vector<lob::BotOrder> stream;
    stream.reserve(kStreamLength);
    for (std::size_t i = 0; i < kStreamLength; ++i) stream.push_back(bot.next());

    lob::MatchingEngine engine("NOALLOC", kMaxPrice);
    std::array<lob::Trade, 64> trades;

    g_counting.store(true, std::memory_order_relaxed);
    for (const lob::BotOrder& order : stream) {
        switch (order.action) {
            case lob::BotAction::MARKET:
                engine.addMarketOrder(order.id, order.side, order.quantity, trades);
                break;
            case lob::BotAction::LIMIT:
                engine.addLimitOrder(order.id, order.side, order.price, order.quantity, trades);
                break;
            case lob::BotAction::CANCEL:
                engine.cancelOrder(order.id);
                break;
        }
    }
    g_counting.store(false, std::memory_order_relaxed);

    std::size_t allocs = g_alloc_count.load(std::memory_order_relaxed);
    std::cout << "hot-path allocations: " << allocs << "\n";
    CHECK(allocs == 0);

    if (lob::test::failureCount() != 0) {
        std::cout << lob::test::failureCount() << " check(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed";
    return 0;
}
