# LOB Matching Engine

A single-symbol, price-time-priority (FIFO) limit order book matching engine
in C++20, built for low and predictable latency.

## Design

- **No heap allocation on the hot path.** Resting orders live in a single
  pre-allocated `ObjectPool<Order, N>` (`include/lob/object_pool.hpp`), a
  fixed-capacity free-list allocator backed by one `std::array`. Order
  insertion, matching, and cancellation only manipulate indices into that
  array; the only heap allocations happen once at engine construction
  (the order pool, the two price-level arrays, and the reserved id→index
  map).
- **O(1) price levels.** Prices are discretized ticks used directly as an
  index into `std::array<PriceLevel, kMaxPriceTicks>` (`include/lob/price_level.hpp`),
  owned via `unique_ptr` the same way the order pool is — a sparse array, not
  a `std::map`, so locating a level never involves a tree walk or hashing.
  `kMaxPriceTicks` (100,000) is a compile-time capacity; a given engine's
  usable range is bounded at runtime by its `max_price` constructor
  argument, which must be `<= kMaxPriceTicks`.
- **O(1) insert/cancel.** Each `PriceLevel` is the head/tail of an intrusive
  doubly linked list of `Order` nodes (linked by pool index, not pointer),
  giving O(1) FIFO append and O(1) removal from anywhere in the list.
  Cancellation resolves an `OrderId` to a pool index via an
  `unordered_map` reserved up front.
- **Cache-friendly `Order`.** `include/lob/order.hpp` is a flat, 32-byte,
  `alignas(32)` POD — id, price, quantity, side, and two link indices — with
  no indirection, so scanning a price level stays in a few cache lines.
- **Best bid/ask tracking.** The engine caches the best bid/ask and only
  rescans neighboring ticks when the current best level empties out, which
  is amortized O(1) for realistic order flow.

<!-- The one simplicity trade-off: `id_to_index_` is a
`std::unordered_map` rather than a hand-rolled open-addressing table, so
cancellation can allocate internally under sustained load even though it's
reserved at engine construction. Swapping in a custom flat hash map would
close that gap if truly zero allocation is required. -->


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
include/lob/       Public headers (types, Order, ObjectPool, PriceLevel, MatchingEngine, TradingBot)
src/                Implementation + demo (main.cpp)
cli/                Interactive REPL for submitting orders and inspecting the book
tests/              Assert-based correctness tests (price-time priority, sweeps, cancel/reuse)
bench/              Google Benchmark harness (ns/op)
```

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/lob_demo          # runs the trading bot against the engine
./build/lob_cli           # interactive REPL: submit orders, inspect the book
ctest --test-dir build    # correctness tests
./build/lob_bench         # latency benchmarks (ns/order); requires network to fetch Google Benchmark once
```

Sanitizer builds:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -DBUILD_BENCHMARKS=OFF
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DBUILD_BENCHMARKS=OFF
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure
```

## Benchmarks

Single core, Apple M5 Pro (15 cores, 64 KiB L1d, 8 MiB L2), macOS <VERSION>,
<COMPILER>, `-O3 -march=native`. Google Benchmark, single run per configuration,
no repetitions — the machine carried a load average of 1.4–2.8, and repeated
runs of an unchanged configuration vary by ~3%, so **treat every figure as ±5%**.

### Results

| benchmark | orders | ns/order | orders/sec |
|---|---:|---:|---:|
| Mixed limit/market/cancel flow | 1,048,576 | **26.8** | **37.4M** |
| Insert + cancel round trip | 31,657,448 | **22.9** | **43.6M** |

The mixed flow replays a pre-generated stream from `TradingBot` (~75% limit,
~15% cancel, ~10% market) with the iteration count pinned below the order
pool's 2²⁰ capacity. The round trip measures pure bookkeeping — pool
acquire/release, intrusive list splice/unlink, and two `id_to_index_`
operations — with no matching.

### How the harness got to those numbers

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