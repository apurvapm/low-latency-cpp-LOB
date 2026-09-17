// Randomised operations checked against std::unordered_map as a reference.
// The table is deliberately sized small relative to the key range so
// collisions cluster and probe chains routinely wrap around the end of the
// table, exercising insert/find/erase (including backward-shift deletion
// from the middle of a chain) under those conditions.

#include <cstdint>
#include <iostream>
#include <random>
#include <unordered_map>

#include "check.hpp"
#include "lob/flat_id_map.hpp"

using namespace lob;

int main() {
    constexpr std::size_t kOrderCapacity = 128; // table capacity -> 256 slots
    constexpr std::size_t kOps = 1'000'000;
    constexpr OrderId kKeyRange = 200; // > half the live budget: forces clustering

    FlatIdMap flat(kOrderCapacity);
    std::unordered_map<OrderId, PoolIndex> ref;
    ref.reserve(kOrderCapacity);

    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<int> op_pick(0, 2); // 0=insert, 1=erase, 2=find
    std::uniform_int_distribution<std::uint64_t> key_pick(0, kKeyRange - 1);
    std::uniform_int_distribution<std::uint32_t> idx_pick(0, static_cast<std::uint32_t>(kOrderCapacity - 1));

    for (std::size_t step = 0; step < kOps; ++step) {
        int op = op_pick(rng);
        OrderId id = key_pick(rng);

        if (op == 0) {
            if (ref.size() >= kOrderCapacity) continue; // keep load factor <= 0.5, as the engine does
            PoolIndex idx = idx_pick(rng);
            bool ref_inserted = ref.emplace(id, idx).second;
            bool flat_inserted = flat.insert(id, idx);
            CHECK(ref_inserted == flat_inserted);
        } else if (op == 1) {
            bool ref_erased = ref.erase(id) != 0;
            bool flat_erased = flat.erase(id);
            CHECK(ref_erased == flat_erased);
        } else {
            auto it = ref.find(id);
            PoolIndex expected = (it == ref.end()) ? kInvalidIndex : it->second;
            CHECK(flat.find(id) == expected);
        }
        CHECK(flat.size() == ref.size());
    }

    // Full-table comparison via forEach.
    std::unordered_map<OrderId, PoolIndex> collected;
    flat.forEach([&](OrderId id, PoolIndex idx) { collected[id] = idx; });
    CHECK(collected.size() == ref.size());
    for (const auto& [id, idx] : ref) {
        auto it = collected.find(id);
        CHECK(it != collected.end() && it->second == idx);
    }

    if (lob::test::failureCount() != 0) {
        std::cout << lob::test::failureCount() << " check(s) failed\n";
        return 1;
    }
    std::cout << "All tests passed";
    return 0;
}
