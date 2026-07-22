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