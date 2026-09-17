# LOB Matching Engine

![CI](https://github.com/apurvapm/low-latency-cpp-LOB/actions/workflows/ci.yml/badge.svg)

A single-symbol, price-time-priority (FIFO) limit order book matching engine
in C++20, built for low and predictable latency.

## Results

Measured on — Intel Core i7-1250U (12th gen,
hybrid 2P+8E cores), Windows 11, GCC 16.1 (MSYS2 ucrt64), `-O3 -march=native`.

**B0: per-order latency, before vs after a hardening pass** (flat id map +
O(1) bitmap, see [Design](#design)), replaying the same 2²⁰-order synthetic
stream against the `v1.0-synthetic` tag and against current `main`:

| metric (all actions) | before | after | change |
|---|---:|---:|---:|
| mean  | 121.5 ns | 86.5 ns | -29% |
| p50   | 61.7 ns  | 36.1 ns | -41% |
| p99   | 617.0 ns | 253.0 ns | -59% |
| p99.9 | 936.4 ns | 416.7 ns | -55% |

The tail moved the most — the flat map (removes a per-insert allocation) and
the bitmap (removes an O(gap) scan) are both tail-latency fixes that do not quantitatively show up in a mean.

**Current Google Benchmark numbers** (median of 5 runs):

| benchmark | ns/op |
|---|---:|
| Mixed limit/market/cancel flow (`BM_MatchingEngineOrderFlow`) | 134 |
| ...stream pre-generated (`BM_MatchingEngineOrderFlow_no_bot`) | 63.4 |
| Insert + cancel round trip (level never empties) | 82.9 |
| Insert + cancel, level always empties (`BM_InsertCancelEmptyLevel`) | 85.3 |

`BM_InsertCancelEmptyLevel` is new: same as the round trip but without the
second "keeper" order, so every cancel empties the level and exercises
`advanceBestBid`/`advanceBestAsk`. It's now within ~3% of the non-emptying
variant — before the bitmap this gap was ~2x (45.2 ns vs 22.9 ns).

Full breakdown by action (limit/market/cancel), raw output, machine details,
and timer overhead: [`docs/results_engine_hardening.md`](docs/results_engine_hardening.md).
For the design behind these numbers, see below; for the pre-hardening
numbers on the original benchmarking machine, see
[Benchmarks](#benchmarks).

## Design

- **No heap allocation on the hot path.** Resting orders live in a
  runtime-sized `ObjectPool<T>` (`include/lob/object_pool.hpp`), a free-list
  allocator backed by one `unique_ptr<T[]>` sized to the engine's
  `order_capacity` at construction. Order insertion, matching, and
  cancellation only manipulate indices into that array; the only heap
  allocations happen once at construction (the order pool, the two
  price-level arrays, the id map, and the two level bitmaps). Enforced by
  `tests/test_no_alloc.cpp`, which overrides the global allocator and
  asserts zero allocations replaying a 2²⁰-order mixed stream.
- **Flat open-addressing id map.** `OrderId -> PoolIndex` resolves through
  `include/lob/flat_id_map.hpp`: linear probing, power-of-two capacity,
  backward-shift deletion (no tombstones), preallocated once at
  construction. This replaces a `std::unordered_map`, whose `reserve()`
  only preallocates the bucket array, not the nodes — every resting insert
  still allocated a node internally despite the reserve (549,602
  allocations measured over the benchmark's 2²⁰-order stream). Validated
  against `std::unordered_map` as a reference over 1M randomized operations
  in `tests/test_flat_id_map.cpp`.
- **O(1) price levels.** Prices are discretized ticks used directly as an
  index into `std::array<PriceLevel, kMaxPriceTicks>` (`include/lob/price_level.hpp`),
  owned via `unique_ptr` the same way the order pool is — a sparse array, not
  a `std::map`, so locating a level never involves a tree walk or hashing.
  `kMaxPriceTicks` (100,000) is a compile-time capacity; a given engine's
  usable range is bounded at runtime by its `max_price` constructor
  argument, which must be `<= kMaxPriceTicks`.
- **O(1) insert/cancel/best-price update.** Each `PriceLevel` is the
  head/tail of an intrusive doubly linked list of `Order` nodes (linked by
  pool index, not pointer), giving O(1) FIFO append and O(1) removal from
  anywhere in the list. When the best bid/ask level empties, the next best
  price is found via a three-level bitmap (`include/lob/level_bitmap.hpp`:
  one bit per tick, a summary bit per 64-tick word, a summary bit per
  64-word group) — a small constant number of `countr_zero`/`countl_zero`
  calls regardless of the gap, not the tick-by-tick scan this used to be.
  Validated against a brute-force scan in `tests/test_level_bitmap.cpp`.
- **Cache-friendly `Order`.** `include/lob/order.hpp` is a flat, 32-byte,
  `alignas(32)` POD — id, price, quantity, side, and two link indices — with
  no indirection, so scanning a price level stays in a few cache lines.
- **Input validation without asserting on the hot path.** An out-of-range
  price or zero quantity is rejected and counted (`rejectedCount()`)
  instead of asserted away; pool exhaustion drops the resting remainder and
  counts it (`droppedCount()`); fills beyond the caller's `trades` span
  still execute but are counted (`truncatedTradeCount()`) instead of
  silently lost; a duplicate order id is rejected and counted
  (`duplicateIdCount()`) instead of overwriting the first order's map
  entry.
- **Fixed-size symbol.** The engine copies its symbol into an 8-byte buffer
  at construction instead of holding a `string_view`, so it can't dangle if
  built from a temporary string.

## Benchmarks

The numbers below predate the hardening pass and were measured at the
`v1.0-synthetic` tag, on the original benchmarking machine (15 cores,
64 KiB L1d, 8 MiB L2) — a different machine than [Results](#results) above,
so the two are **not directly comparable** to each other.

### Results (`v1.0-synthetic`)

Google Benchmark, single run per configuration, `-O3 -march=native`.

| benchmark | orders | ns/order | orders/sec |
|---|---:|---:|---:|
| Mixed limit/market/cancel flow | 1,048,576 | **26.8** | **37.4M** |
| Insert + cancel round trip | 31,657,448 | **22.9** | **43.6M** |

The mixed flow replays a pre-generated stream from `TradingBot` (~75% limit,
~15% cancel, ~10% market) with the iteration count pinned below the order
pool's 2²⁰ capacity. The round trip measures pure bookkeeping — pool
acquire/release, intrusive list splice/unlink, and two `id_to_index_`
operations — with no matching.

## CLI

`lob_cli` is an interactive REPL over a single `MatchingEngine`. Prices are
entered and displayed as decimal dollars ("100.25"), with a $0.01 tick size —
internally the engine still works in integer ticks (`Price` in
`[0, 100000)`, i.e. up to $999.99), matched exactly via integer arithmetic in
`parsePrice`/`formatPrice` (`cli/lob_cli.cpp`) with no floating-point
rounding. Order ids are assigned automatically and echoed back so you can
cancel them later.

```
$ ./build/lob_cli
> sell 100.25 10
Order #1: SELL 10 @ 100.25
  resting 10 @ 100.25

> buy 100.25 4
Order #2: BUY 4 @ 100.25
  filled 4 @ 100.25 (maker #1)

> book
ASKS (price  qty  orders)
  100.25  6  1
  ---- spread: - / 100.25 ----
BIDS (price  qty  orders)

> buy market 3
Order #3: BUY 3 @ MARKET
  filled 3 @ 100.25 (maker #1)

> orders
id  side  price  qty
#1  SELL  100.25  3

> cancel 1
Cancelled order #1

> quit
```

Type `help` at the prompt for the full command list (`buy`/`sell`, market
variants, `cancel`, `book [depth]`, `orders`, `trades [n]`).

## Trading bot

`TradingBot` (`include/lob/trading_bot.hpp`) generates a synthetic order flow
around a mean price using a discretized mean-reverting walk, mixing limit
orders, market orders, and cancels of its own resting orders. `lob_demo` and
the benchmark harness both drive the engine with it.

## Layout

```
include/lob/        Public headers (types, Order, ObjectPool, PriceLevel, FlatIdMap, LevelBitmap, MatchingEngine, TradingBot)
src/                 Implementation + demo (main.cpp)
cli/                 Interactive REPL for submitting orders and inspecting the book
tests/               CHECK-based correctness tests (checked in every build type, including Release)
bench/               Google Benchmark harness (ns/op) + lob_latency (rdtsc percentile harness)
docs/                Design spec and measured results
.github/workflows/   CI: Release / Debug+ASan+UBSan / Debug+TSan, each build+test on ubuntu-latest
```

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/lob_demo          # runs the trading bot against the engine
./build/lob_cli           # interactive REPL: submit orders, inspect the book
ctest --test-dir build    # correctness tests (order book, flat map, bitmap, allocation-free hot path)
./build/lob_latency       # per-order latency percentiles (rdtsc), not Google Benchmark
./build/lob_bench         # Google Benchmark harness (ns/order); requires network to fetch it once
```

Sanitizer builds (CI runs these on ubuntu-latest; on Windows/MinGW the
toolchain used in this session ships the sanitizer headers but not their
runtime libraries, so they can only be linked and run on Linux):

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DBUILD_BENCHMARKS=OFF
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DBUILD_BENCHMARKS=OFF
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure
```


### Benchmark details

The first version of the mixed-flow benchmark reported 95.9 ns/order. Two
separate measurement faults accounted for most of that:

| configuration | ns/order | what it was measuring |
|---|---:|---|
| unpinned (11.1M orders), order generation inside the timed loop | 95.9 | RNG **and** a saturated order pool |
| pinned to 2¹⁹, generation still in the loop | 69.5 | RNG plus the engine |
| pinned to 2¹⁹, stream pre-generated | 25.8 | the engine |

- **Order generation dominated the measurement.** `TradingBot::next()` draws
  from a `normal_distribution` (a `log`/`sqrt` each), two `uniform_int`s, a
  `bernoulli`, and a `uniform_real` over `mt19937_64` — ~44 ns of the 69.5 ns,
  i.e. 63% of the reported latency was the generator, not the matcher.
  Pre-generating the stream moves it out of the timed region.
- **An unbounded iteration count saturated the order pool.** At ~75% limit
  orders, 11.1M orders drive ~8.5M insertions against a 1,048,576-slot pool.
  Occupancy does not merely exhaust — market orders and cancels keep returning
  slots that the next non-crossing limit immediately claims, so the run parks
  at ~100% occupancy, where `id_to_index_` sits at load factor ~1.0 and the
  free list is fully scrambled. That degradation costs *more* than the
  skipped inserts save, which is why the unpinned run is slower despite doing
  less work. Pinning iterations below pool capacity fixes it.

Both remaining figures are averages over a book growing from empty, not a
steady state. A steady-state measurement would warm the book to a target depth
outside the timed region and use a cancel rate that holds it there; ~400k
resting orders across ~70 live ticks is far deeper than any real single-symbol
book.

`BM_InsertCancelRoundTrip` originally reported 45.2 ns. Cancelling the only
order at the best bid empties the level, which calls `advanceBestBid()` — a
linear scan from tick 99 down to 0. That rescan was ~22 ns, half the
measurement. Keeping a second resting order at the same price so the level
never empties isolates the O(1) path: 45.2 → 22.9 ns.

### Raw `lob_bench` output

```
Benchmark                                                    Time             CPU   Iterations UserCounters...
--------------------------------------------------------------------------------------------------------------
# unpinned, order generation inside the timed loop
BM_MatchingEngineOrderFlow                                95.9 ns         95.8 ns     11130191 items_per_second=10.4417M/s
BM_MatchingEngineOrderFlow_no_bot/iterations:524288       25.0 ns         25.0 ns       524288 items_per_second=40.0556M/s
BM_InsertCancelRoundTrip                                  23.1 ns         23.1 ns     30573961 items_per_second=43.2603M/s

# both mixed-flow variants pinned to 2^19
BM_MatchingEngineOrderFlow/iterations:524288              69.5 ns         69.5 ns       524288 items_per_second=14.3964M/s
BM_MatchingEngineOrderFlow_no_bot/iterations:524288       25.8 ns         25.8 ns       524288 items_per_second=38.7271M/s
BM_InsertCancelRoundTrip                                  22.0 ns         22.0 ns     31842353 items_per_second=45.3892M/s

# both pinned to 2^20
BM_MatchingEngineOrderFlow/iterations:1048576             65.6 ns         65.6 ns      1048576 items_per_second=15.2545M/s
BM_MatchingEngineOrderFlow_no_bot/iterations:1048576      26.8 ns         26.7 ns      1048576 items_per_second=37.4091M/s
BM_InsertCancelRoundTrip                                  22.9 ns         22.9 ns     31657448 items_per_second=43.6423M/s
```

`items_per_second` is `SetItemsProcessed(state.iterations())`, so it is exactly
the reciprocal of the Time column — the same measurement in different units.

`BM_InsertCancelRoundTrip` is unpinned throughout: it cancels every order it
inserts, so pool occupancy never exceeds two and the run is safe at any
iteration count. Its stability across all three runs (23.1 / 22.0 / 22.9 ns)
is the control that shows the variation in the other two is the book, not the
harness.

## Project history

- **v1.0** (`v1.0-synthetic`): initial engine. Correct matching, but the id
  map allocated on every insert and the best-price scan was O(gap).
- **v1.1**: engine hardening. Allocation-free id map, O(1) bitmap best-level
  search, input validation counters, CI with ASan/UBSan/TSan. p99 latency
  down 59% on the same synthetic stream (see [Results](#results)).