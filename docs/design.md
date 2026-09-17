# lob-engine v2 — Real-Data Replay & Tail Latency: Build Spec

Extends the existing `low-latency-cpp-LOB` engine in place. Adds three things:

1. **Real market data.** Replay a full day of NASDAQ TotalView-ITCH 5.0 through the engine.
2. **A two-thread pipeline.** A parser thread and an engine thread, joined by a lock-free SPSC ring buffer.
3. **Tail-latency measurement.** p50 / p99 / p99.9 / max from histograms, instead of an average.

**Two threads, one ring, one writer per data structure. No networking, no persistence.**

Before touching anything: `git tag v1.0-synthetic`. The resume's 37M orders/s (26.8 ns/order)
figure must stay reproducible from that tag.

---

## Findings in the current engine (fix first: Day 0)

Checked against `44ece64`. Three of the resume's current claims don't hold for the code as it
stands:

| Claim | Reality | Evidence |
|---|---|---|
| "zero heap allocation on the hot path" | `id_to_index_` is a `std::unordered_map`. `reserve()` preallocates only the bucket array, not the nodes, so every resting insert allocates and every erase frees. | Replaying the benchmark's 1,048,576-order stream: **549,602 allocations and 394,987 frees** inside the timed loop. |
| "O(1) insert, cancel, and match" | When the best level empties, `advanceBestBid/Ask()` scans tick by tick, which is O(gap), up to 100,000 ticks. The README already shows it costing about 22 ns on the round-trip benchmark. | `matching_engine.cpp:144–168` |
| "ASAN/UBSAN builds" | CMake has `ENABLE_ASAN` and `ENABLE_TSAN`, but no UBSan option. | `CMakeLists.txt` |

Two further problems also matter for real data:

- **Price range is checked only by `assert`.** `addLimitOrder` asserts `0 <= price < max_price`.
  In Release builds that check is removed, so a price out of range (e.g. an ITCH stub ask at
  $199,999.99) writes past the end of the level array. That is undefined behaviour.
- **Tests only check anything in Debug builds.** `tests/test_order_book.cpp` uses bare `assert`
  (22 of them), and the default build type is Release, which defines `NDEBUG`. So
  `ctest --test-dir build` passes without checking anything. The sanitizer builds use Debug, so
  those do test.

Also worth fixing:

- **Pool size is fixed and large.** `ObjectPool<Order, 1<<20>` is a compile-time capacity: about
  36 MB per engine, and the constructor touches all of it. Fifty symbols would need about 1.8 GB.
- **`symbol_` is a `std::string_view`.** It dangles if the engine is built from a temporary
  string, which is likely when names come from ITCH `R` messages.
- **Trade recording is silently truncated.** `match()` records at most `trades.size()` fills but
  keeps filling orders, and the caller can't tell the list was cut short.

### Day 0 changes

1. **Flat hash map.** Replace `id_to_index_` with a preallocated open-addressing map from
   `OrderId` to `PoolIndex` (linear probing, power-of-two capacity, backward-shift deletion). This
   makes the allocation claim true.
2. **Hierarchical bitmap per side.** Use one bit per tick, plus a summary word per 64 words
   (100,000 ticks need 1,563 words, then 25 summary words, then one top word). Finding the next
   non-empty level becomes a few `std::countr_zero` / `std::countl_zero` calls, a small constant
   number of steps at any gap. This makes the O(1) claim true.
3. **Explicit price check.** An out-of-range price returns a rejection that callers can count,
   instead of relying on `assert`.
4. **Runtime-sized `ObjectPool`.** One `unique_ptr<T[]>` allocated at construction.
5. **Fixed-size symbol.** `symbol_` becomes a `char[8]` array (ITCH symbols are 8 bytes).
6. **Real test checks.** A `CHECK` macro that works in every build type, an `ENABLE_UBSAN`
   option, and a GitHub Actions workflow running the tests under ASan+UBSan and TSan.
7. **Allocation-count test.** The counting `operator new` check used above, run as a CTest over
   the mixed flow, asserting **zero** allocations after warm-up.
8. **Re-run the synthetic benchmark** and update the README and resume with the new numbers. Also
   report the per-order latency distribution (p50/p99/max), not just the mean; the scan and
   allocation fixes should mainly improve the tail.

`v1.0-synthetic` keeps the old numbers reproducible. The new ones are tagged `v1.1`.

---

## Pipeline

```
ITCH file (mmap) ──► [parser thread] ──SPSC ring──► [engine thread]
                     decode + normalise              order-ref lookup + book update
                     stamp t0                        route by locate → per-symbol engine
                                                     stamp t1, record (t1 − t0)
```

- Both threads are pinned to separate **physical** cores (not SMT siblings; check `lscpu -e`).
- The ring carries a fixed-size normalised `Cmd` (below), never raw ITCH bytes.
- The engine thread is the only writer to the books and their ID maps. The parser only reads the
  file.

---

## Data

- Source: NASDAQ's public ITCH 5.0 sample files (`emi.nasdaq.com/ITCH/`), one full trading day.
  Compressed files are several GB; decompressed files are larger still. Check disk space first.
- `scripts/fetch_itch_sample.sh` downloads and decompresses. Data is **never committed**
  (`.gitignore`).
- File framing: every message is preceded by a **2-byte big-endian length**.
- All integers are big-endian. The timestamp is 6 bytes (nanoseconds since midnight). Prices are
  `uint32` with 4 implied decimal places.

### Messages handled

Every message starts with the same 11-byte header: type (1), stock locate (2), tracking number (2),
timestamp (6). Verify all lengths against the spec PDF, and have the parser assert them.

| Type | Meaning | Length | Book action |
|---|---|---|---|
| `R` | Stock directory | 39 | locate → symbol table; select universe |
| `A` | Add order | 36 | insert resting order |
| `F` | Add order with MPID | 40 | same as `A` |
| `E` | Order executed | 31 | reduce the referenced order |
| `C` | Executed with price | 36 | same as `E` (the price is informational) |
| `X` | Partial cancel | 23 | reduce the referenced order, **keeping priority** |
| `D` | Order delete | 19 | remove the referenced order |
| `U` | Order replace | 35 | delete the old ref, add the new ref (**loses priority**; side and locate come from the old order) |
| `S` | System event | 12 | market open/close markers only |
| other | trades, NOII, crosses, ... | by prefix | skip using the length prefix; count per type |

### Universe

- Pre-pass: count messages per stock locate and pick the **top K symbols (K = 50)** by message
  count. Record the list in the README.
- The engine's dense level array covers $0.00–$999.99 in whole cents, so only stocks trading
  between **$1 and $1,000** (judged by first-add price in the pre-pass) are selected. At ≥ $1,
  displayed prices are whole cents: convert with `price / 100`, assert `price % 100 == 0`, and
  count any violations rather than silently accepting them.

---

## Parser (zero-copy)

- `mmap` the decompressed file with `madvise(MADV_SEQUENTIAL)`. Walk it using the length
  prefixes.
- Decode fields with `memcpy` into a local integer followed by a byte-swap
  (`__builtin_bswap16/32/64`). No `reinterpret_cast` onto packed structs, since that's
  undefined behaviour from aliasing and alignment.
- 6-byte timestamps: load 2 + 4 bytes and combine them.
- Filter by stock locate **before** pushing to the ring. Messages outside the universe cost only
  a header read.
- Output is a normalised command:

```cpp
struct Cmd {                 // 32 bytes, trivially copyable
    uint64_t ref;            // ITCH order reference number == engine OrderId
    uint64_t new_ref;        // U only
    uint32_t price;          // converted to cents (engine ticks) by the parser
    uint32_t qty;
    uint16_t locate;
    uint8_t  type;           // normalised: ADD / EXEC / REDUCE / DEL / REPLACE
    uint8_t  side;
    uint32_t t0_lo;          // low 32 bits of the parse TSC; high bits are implied
};
```

  Adjust the field layout if a measurement needs the full 64-bit t0. Check the size with
  `static_assert(sizeof(Cmd) == 32)`.

- Truncated or corrupt input (a length prefix that runs past the end of the file, or a wrong
  length for a known type) leads to a clean error, never undefined behaviour.

---

## Engine-side changes (for replay, after Day 0)

**No separate order-ref map.** ITCH order refs are unique for the whole day and fit the engine's
64-bit `OrderId`, so they are used directly as engine IDs. Every `E`/`C`/`X`/`D`/`U` message
carries the stock locate in its header, which routes it to the right engine. The Day 0 flat map
inside each engine handles the ID lookup. Size each engine's map and pool from that symbol's peak
live-order count in the pre-pass (×2, rounded up to a power of two).

**Engine API additions**, each with unit tests. Keep them O(1) and allocation-free:
- `restOrder(id, side, price, qty)`: insert **without matching**. ITCH adds have already rested
  on NASDAQ, so if an add would cross our book, that signals a divergence. Count it and insert
  anyway.
- `reduceOrder(id, qty)`: partial cancel or partial execution that **keeps queue position**.
  Removes the order when its quantity reaches zero.
- `frontOrder(side, price) → OrderId`: head of a price level. Used for the priority-agreement
  check below.
- `U` (replace) is `cancelOrder(old)` followed by `restOrder(new, same side, ...)`, taking the
  side from the old order before removing it.

**One engine per symbol**, stored in an array indexed by stock locate (`uint16`). Only the
selected symbols get an instance.

**Stub quotes.** Market makers leave orders at extreme prices far from the market. With the
Day 0 bitmap, low stub bids inside $0–$999.99 cost nothing extra. Asks at ≥ $1,000 are outside
the array, so they go into a small preallocated **overflow table** keyed by `OrderId`, so later
`D`/`X`/`U` messages can still find them. They never trade in practice. Report how many there
were. This is one of the main things real data shows that synthetic data doesn't.

**Warm-up.** Touch every pool, map, level and bitmap page before timing starts (pre-fault).
Discard the first N messages from all latency statistics.

---

## SPSC ring buffer (`spsc/`, header-only)

- Power-of-two capacity, stored in a single contiguous array.
- Head and tail on separate cache lines (`alignas(64)` plus padding), so the two threads don't
  cause false sharing.
- **Cached copy of the other side's index**: the producer keeps a local copy of the head and
  re-reads the shared value only when the ring appears full; the consumer does the same with the
  tail. This is the main optimisation. Measure it by ablation.
- Each thread writes its own index with `store(release)` and reads the other's with
  `load(acquire)`.
- The consumer dequeues in batches (`try_pop_n`). Measure the effect of batch size.
- Wait strategy: busy-spin with `_mm_pause()` by default, plus a blocking variant for comparison.
- Tests:
  - wrap-around, full, empty, and capacity 1 or 2;
  - a two-thread stress test pushing sequence numbers that checks for loss, duplicates and
    reordering;
  - all of it run under **TSan** (plus ASan and UBSan builds).

---

## Measurement

**Clock.** Use `rdtsc`, and require invariant TSC (`constant_tsc` and `nonstop_tsc` in
`/proc/cpuinfo`). Calibrate TSC ticks per nanosecond against `clock_gettime` at startup.
**Measure the cost of the timer itself** (an empty timed region) and report it next to every
latency table. The engine's work per message is only tens of nanoseconds, so the timer's cost
matters.

**Histograms.** Use HdrHistogram_c (vendored) or an equivalent log-linear histogram. Report
p50 / p90 / p99 / p99.9 / p99.99 / max. **Never report averages.** Break the results down by
message type (add / exec / reduce / delete / replace).

### Benchmarks

| # | Benchmark | What it answers |
|---|---|---|
| B0 | Synthetic mixed flow before vs after Day 0 fixes (per-order p50/p99/max + mean) | What the flat map and bitmap buy, especially in the tail |
| B1 | Parse-only pass | Parser throughput (messages/s, GB/s) |
| B2 | Engine alone on pre-decoded `Cmd`s in memory (universe only), single thread | Per-message latency on **real** data, compared with the synthetic 26.8 ns |
| B3 | Pipeline at maximum speed | End-to-end throughput; ring occupancy |
| B4 | **Paced open-loop replay** at 1×, 10× and 100× the original ITCH timing, plus max | Latency against load (the "hockey-stick" chart) |
| B5 | Queue ablation: SPSC vs mutex + condvar; cached index on vs off; batch sizes; spin vs block | Where the pipeline's performance comes from |
| B6 | `perf stat` for engine alone, real vs synthetic: cache misses, branch misses, instructions per message | Why real data differs from synthetic |

**Coordinated omission (B4).** Each message's *intended* send time is its ITCH timestamp mapped
onto wall-clock time at the chosen speed-up. Latency is measured from that intended time, not
from the time the parser actually pushed the message. When the pipeline falls behind, the
backlog then shows up in the tail instead of being hidden. The README should explain this in two
sentences.

**Environment**, recorded in the README with every result table: CPU model, core IDs used,
compiler and flags (`-O3 -march=native`), CPU frequency governor, turbo on or off, and kernel
version. Run each benchmark at least 3 times and report the median run. Flag any run that differs
from the others by more than 10%.

---

## Correctness

- **Differential book test.** A deliberately simple reference book (`std::map` levels,
  `std::list` orders) processes the same command stream. Every M messages, and at the end of the
  day, assert that the top-N levels of both books are identical for every symbol.
- **Book invariants**, checked in debug builds after each message:
  - the book is never crossed;
  - no quantity is negative;
  - every referenced ITCH order ref exists;
  - end-of-day counts (adds, executions, deletes) agree with the parser's per-type counts.
- **Priority agreement.** Before applying each `E` or `C` message, call `front(opposite level)`
  and check whether our price-time priority would have filled the same order ITCH reports.
  Report the agreement rate. **Mismatches are expected** (hidden or reserve orders, special
  order types), so categorise them rather than hide them.
- **Pipeline determinism.** The two-thread run's output log (the sequence of book changes) must
  hash identically to a single-thread run on the same stream. This proves the ring never
  reorders or drops messages.
- **Parser tests.** Hand-built big-endian frames for every handled type; truncated files;
  unknown types skipped via the length prefix.
- **libFuzzer** on the parser: malformed or truncated input must never crash.
- Carry forward the existing ASan / UBSan / TSan CMake options, and run all tests in CI on a
  **synthetic ITCH file** produced by a small ITCH writer (`itch/writer`), never on NASDAQ data.
  The sample files' terms don't clearly allow redistributing them, so no excerpt is committed.

---

## Repo layout

```
low-latency-cpp-LOB/
├── include/lob/   # existing engine headers + flat_id_map, level_bitmap; engine depends on nothing else
├── src/           # existing engine sources
├── itch/          # ITCH 5.0 parser + writer + tests + fuzz target
├── spsc/          # header-only ring + mutex baseline + tests
├── apps/replay/   # universe pre-pass, single-thread replay, two-thread pipeline, paced replay
├── bench/         # existing Google Benchmark + lob_latency + B1–B6
├── tests/         # existing tests + flat map, bitmap, no-alloc, differential, determinism
├── scripts/       # fetch data, plot charts
├── docs/          # design.md, universe.md, results_*.md, img/
└── README.md
```

The existing `include/` + `src/` layout stays; new components go beside it.

---

## Order of work

| Step | Deliverable | Done when |
|---|---|---|
| A | Day 0 fixes, CI, allocation test, **B0** | Zero allocations asserted in CI; README numbers re-measured |
| B | Fetch script, parser, writer, universe pre-pass, fuzz target, **B1** | Full day parses; per-type counts printed |
| C | Engine API additions, stub-quote overflow, per-symbol replay, differential test, invariants, priority agreement, **B2** | Differential test passes on the full day; per-type latency table on real data |
| D | SPSC + TSan stress + mutex baseline + pipeline + determinism test, **B3** | Two-thread hash equals single-thread hash |
| E | **B4** (open-loop), **B5**, **B6**, charts | Latency-vs-load chart, ablation table, `perf stat` table |
| F | README, final summary | Every number reproducible with one command |

Correctness comes before speed at every step: a number without the differential test behind it
is not quoted.

---

## Explicit non-goals

Networking · persistence / WAL · order entry protocol · hidden, midpoint and auction orders
(`Q`, `I`, NOII) beyond counting them · multiple venues · kernel bypass · a trading strategy ·
matching-mode replay (turning executions into incoming aggressive orders), since the engine's
book would diverge from ITCH at the first priority mismatch.

---

## Framing

- The README opens with: *a price-time-priority matching engine replaying a full NASDAQ ITCH 5.0
  day, with per-message tail latency and a lock-free two-thread pipeline.* Follow it with the
  results table and the environment block.
- Resume bullet template. Fill it only from measured results, and use `X` until then:

```latex
\item Replayed a full \textbf{NASDAQ ITCH 5.0} day (\textbf{X M} messages, top-50 symbols)
      via a zero-copy parser; books matched a reference implementation at every checkpoint,
      with \textbf{p50/p99/p99.9 of X/X/X\,ns} per message.
\item Split feed handling and matching across pinned cores with a \textbf{lock-free SPSC ring}
      (cached indices, batch drain): \textbf{X M msgs/s}, \textbf{X$\times$} over a mutex
      queue, output byte-identical to single-threaded replay.
```

- Be ready to explain in an interview:
  - why averages hide the latency that matters;
  - coordinated omission, and how paced open-loop replay avoids it;
  - why partial cancels keep queue priority but replaces lose it;
  - what stub quotes do to a tick-indexed book, and how the bitmap finds the next level;
  - why `reserve()` doesn't make `std::unordered_map` allocation-free;
  - why the SPSC ring needs no compare-and-swap;
  - why priority agreement is below 100%.
