// B0: per-order latency distribution for the synthetic mixed stream, before
// vs after the Day 0 fixes. Not Google Benchmark -- this times each op with
// rdtsc into a preallocated vector and reports percentiles, since an average
// hides exactly the tail the flat map and bitmap were meant to fix.
//
// Core pinning is done in-process (SetThreadAffinityMask / pthread_setaffinity_np)
// rather than via `taskset`, since this also needs to run on Windows.
// TSC calibration uses std::chrono::steady_clock as the reference wall clock
// (clock_gettime(CLOCK_MONOTONIC) on Linux, QueryPerformanceCounter on
// Windows under the hood) -- same intent as calibrating against
// clock_gettime directly, portably.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#else
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>
#endif

#include "lob/matching_engine.hpp"
#include "lob/trading_bot.hpp"

namespace {
    constexpr lob::Price kMaxPrice = 100'000;
    constexpr lob::Price kMeanPrice = 50'000;
    constexpr std::size_t kStreamLength = 1u << 20;

    void pinToCore(unsigned core) {
#if defined(_WIN32)
        SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(std::uint64_t{1} << core));
#else
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
    }

    inline std::uint64_t rdtsc() {
        return __rdtsc();
    }

    // TSC ticks per nanosecond, calibrated over ~200ms against a monotonic
    // wall clock.
    double calibrateTscPerNs() {
        using namespace std::chrono;
        auto wall_start = steady_clock::now();
        std::uint64_t tsc_start = rdtsc();
        while (duration_cast<milliseconds>(steady_clock::now() - wall_start).count() < 200) {
            // spin
        }
        std::uint64_t tsc_end = rdtsc();
        auto wall_end = steady_clock::now();
        double ns = static_cast<double>(duration_cast<nanoseconds>(wall_end - wall_start).count());
        double ticks = static_cast<double>(tsc_end - tsc_start);
        return ticks / ns;
    }

    struct Stats {
        double p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0, mean = 0;
    };

    Stats computeStats(std::vector<std::uint64_t>& samples) {
        Stats s;
        if (samples.empty()) return s;
        std::sort(samples.begin(), samples.end());
        auto pct = [&](double p) -> double {
            std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
            return static_cast<double>(samples[idx]);
        };
        double sum = 0;
        for (std::uint64_t v : samples) sum += static_cast<double>(v);
        s.p50 = pct(0.50);
        s.p90 = pct(0.90);
        s.p99 = pct(0.99);
        s.p999 = pct(0.999);
        s.p9999 = pct(0.9999);
        s.max = static_cast<double>(samples.back());
        s.mean = sum / static_cast<double>(samples.size());
        return s;
    }

    void printStats(const char* label, std::size_t n, const Stats& s, double tsc_per_ns) {
        std::printf("%-16s n=%9zu  ticks: p50=%8.1f p90=%8.1f p99=%8.1f p99.9=%8.1f p99.99=%8.1f max=%10.1f mean=%8.1f\n",
            label, n, s.p50, s.p90, s.p99, s.p999, s.p9999, s.max, s.mean);
        std::printf("%-16s %9s     ns: p50=%8.2f p90=%8.2f p99=%8.2f p99.9=%8.2f p99.99=%8.2f max=%10.2f mean=%8.2f\n",
            "", "", s.p50 / tsc_per_ns, s.p90 / tsc_per_ns, s.p99 / tsc_per_ns, s.p999 / tsc_per_ns,
            s.p9999 / tsc_per_ns, s.max / tsc_per_ns, s.mean / tsc_per_ns);
    }
}

int main(int argc, char** argv) {
    unsigned core = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--core" && i + 1 < argc) {
            core = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        }
    }
    pinToCore(core);

    double tsc_per_ns = calibrateTscPerNs();
    std::printf("pinned_core=%u tsc_per_ns=%.4f\n", core, tsc_per_ns);

    lob::TradingBot bot(1, kMeanPrice, kMaxPrice);
    std::vector<lob::BotOrder> stream;
    stream.reserve(kStreamLength);
    for (std::size_t i = 0; i < kStreamLength; ++i) stream.push_back(bot.next());

    lob::MatchingEngine engine("LATENCY", kMaxPrice);
    std::array<lob::Trade, 64> trades;

    // Timer overhead: an empty timed region, same sample count as below.
    std::vector<std::uint64_t> overhead_ticks(kStreamLength);
    for (std::size_t i = 0; i < kStreamLength; ++i) {
        std::uint64_t t0 = rdtsc();
        std::uint64_t t1 = rdtsc();
        overhead_ticks[i] = t1 - t0;
    }

    std::vector<std::uint64_t> all_ticks;
    std::vector<std::uint64_t> limit_ticks, market_ticks, cancel_ticks;
    all_ticks.reserve(kStreamLength);
    limit_ticks.reserve(kStreamLength);
    market_ticks.reserve(kStreamLength);
    cancel_ticks.reserve(kStreamLength);

    for (std::size_t i = 0; i < kStreamLength; ++i) {
        const lob::BotOrder& order = stream[i];
        std::uint64_t t0 = rdtsc();
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
        std::uint64_t dt = rdtsc() - t0;
        all_ticks.push_back(dt);
        switch (order.action) {
            case lob::BotAction::LIMIT: limit_ticks.push_back(dt); break;
            case lob::BotAction::MARKET: market_ticks.push_back(dt); break;
            case lob::BotAction::CANCEL: cancel_ticks.push_back(dt); break;
        }
    }

    printStats("timer_overhead", overhead_ticks.size(), computeStats(overhead_ticks), tsc_per_ns);
    printStats("all", all_ticks.size(), computeStats(all_ticks), tsc_per_ns);
    printStats("limit", limit_ticks.size(), computeStats(limit_ticks), tsc_per_ns);
    printStats("market", market_ticks.size(), computeStats(market_ticks), tsc_per_ns);
    printStats("cancel", cancel_ticks.size(), computeStats(cancel_ticks), tsc_per_ns);

    return 0;
}
