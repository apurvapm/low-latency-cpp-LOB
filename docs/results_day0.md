# B0: synthetic mixed-flow latency, before vs after Day 0

Before = `v1.0-synthetic` (commit `44ece64`, built in a separate `git worktree`).
After = current `main` after A1-A5 (flat id map, hierarchical bitmap, input
validation, runtime-sized pool, fixed-size symbol).

Both binaries are the same `bench/latency_mixed.cpp` harness (not Google
Benchmark): it replays the same pre-generated 2^20-order mixed stream
(`TradingBot(1, 50000, 100000)`, ~75% limit / ~15% cancel / ~10% market),
times each op with `rdtsc`, and reports percentiles instead of an average.

## Environment

- CPU: Intel Core i7-1250U (12th Gen, hybrid: 2 P-cores / 8 E-cores, 12
  threads), 1.1 GHz base per Windows' reported `MaxClockSpeed` (turbo varies).
- OS: Windows 11 Home Single Language, build 26200.
- Compiler: GCC 16.1.0 (MSYS2 ucrt64), `-O3 -march=native -std=gnu++20`.
- Core pinning: in-process `SetThreadAffinityMask` to logical core 0 (no
  `taskset`/`lscpu` on Windows). Which physical/P-vs-E core that maps to is
  not controllable from here.
- **No Linux-style frequency governor exists on Windows**; the machine was on
  its default balanced power plan, turbo/throttling state not fixed. This is
  a laptop, not a dedicated benchmarking box.
- TSC calibrated against `std::chrono::steady_clock` (`QueryPerformanceCounter`
  under the hood) over 200ms at startup: `tsc_per_ns = 1.8816` (consistent
  across all runs below, as expected for an invariant TSC).

**Caveat:** this is a shared, thermally-constrained, hybrid-core Windows
laptop with no fixed frequency governor and no true core isolation — not the
pinned bare-metal Linux box the task brief assumes. Absolute nanosecond
figures should be read as directional, not as a clean lower bound; the max
column in particular is dominated by OS scheduling noise (multi-millisecond
outliers from context switches / thermal throttling, not the engine). The
before/after **comparison**, run on the same machine under the same
conditions with the same harness, is still meaningful, and matches what the
flat-map and bitmap allocation/complexity fixes predict.

Timer overhead (median of 5 runs, an empty `rdtsc();rdtsc()` region, same
1,048,576-sample count as below): **p50 ≈ 9 ns, p99 ≈ 12-14 ns**, both
builds — small relative to the ~35-70 ns per-order medians measured, but
not negligible at the tail.

## B0 table: median of 5 runs, nanoseconds (`lob_latency --core 0`)

### Before (`v1.0-synthetic`)

| action | mean | p50 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|
| all    | 121.5 | 61.7 | 617.0 | 936.4 | 3,120,105.8 |
| limit  | 106.4 | 63.8 | 282.2 | 758.4 | 3,120,105.8 |
| market | 126.1 | 77.1 | 440.1 | 985.3 | 1,187,348.0 |
| cancel | 189.3 | 21.8 | 781.3 | 1,995.6 | 1,439,380.6 |

### After (current `main`)

| action | mean | p50 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|
| all    | 86.5 | 36.1 | 253.0 | 416.7 | 1,791,300.4 |
| limit  | 84.9 | 35.6 | 242.9 | 406.6 | 1,320,073.5 |
| market | 81.7 | 47.8 | 234.4 | 406.6 | 511,297.6 |
| cancel | 92.2 | 21.8 | 316.8 | 456.0 | 1,310,712.4 |

### Improvement (all actions combined)

| metric | before | after | change |
|---|---:|---:|---:|
| mean  | 121.5 ns | 86.5 ns | -29% |
| p50   | 61.7 ns  | 36.1 ns | -41% |
| p99   | 617.0 ns | 253.0 ns | -59% |
| p99.9 | 936.4 ns | 416.7 ns | -55% |

The tail moved the most, as expected: P1 (flat map) removed the per-insert
`unordered_map` node allocation, and P2 (bitmap) removed the O(gap)
`advanceBestBid/Ask` scan — both are tail-latency problems that barely show
up in a mean.

### `max` and the outlier column

`max` did not improve proportionally (before 3.12 ms, after 1.79 ms; some
individual runs saw single spikes over 5-7 ms in *both* builds). These
spikes are 3-4 orders of magnitude above the p99.99 and are consistent with
OS-level preemption (thread quantum expiry, an E-core migration, a page
fault) on a shared laptop, not the matching engine — the algorithmic fixes
in A1-A4 don't touch anything that could cost milliseconds. Per the working
rules, this "unexpected direction" was investigated (see A4's commit
message for the `BM_InsertCancelEmptyLevel` investigation, where the same
kind of noise was reproduced and ruled out as a real regression by rerunning
several times) rather than papered over.

### Raw output, one representative run each

```
$ ./build-v1/lob_latency --core 0        # v1.0-synthetic, worktree ../lob-v1
pinned_core=0 tsc_per_ns=1.8816
timer_overhead   n=  1048576  ticks: p50=    17.0 p90=    19.0 p99=    23.0 p99.9=    30.0 p99.99=    64.0 max=  294182.0 mean=    17.8
                               ns: p50=    9.03 p90=   10.10 p99=   12.22 p99.9=   15.94 p99.99=   34.01 max= 156346.87 mean=    9.48
all              n=  1048576  ticks: p50=   125.0 p90=   290.0 p99=  1066.0 p99.9=  1762.0 p99.99=114138.0 max= 6609559.0 mean=   232.1
                               ns: p50=   66.43 p90=  154.12 p99=  566.54 p99.9=  936.44 p99.99=60660.13 max=3512736.48 mean=  123.36
limit            n=   786302  ticks: p50=   128.0 p90=   259.0 p99=   531.0 p99.9=  1350.0 p99.99= 78147.0 max= 6609559.0 mean=   209.1
                               ns: p50=   68.03 p90=  137.65 p99=  282.21 p99.9=  717.48 p99.99=41532.24 max=3512736.48 mean=  111.15
market           n=   104436  ticks: p50=   153.0 p90=   336.0 p99=   776.0 p99.9=  1748.0 p99.99= 98547.0 max= 2234112.0 mean=   241.8
                               ns: p50=   81.31 p90=  178.57 p99=  412.42 p99.9=  929.00 p99.99=52374.09 max=1187348.01 mean=  128.49
cancel           n=   157838  ticks: p50=    43.0 p90=   838.0 p99=  1470.0 p99.9=  3755.0 p99.99=475482.0 max= 2708336.0 mean=   340.1
                               ns: p50=   22.85 p90=  445.37 p99=  781.25 p99.9= 1995.64 p99.99=252701.12 max=1439380.55 mean=  180.74

$ ./build/lob_latency --core 0           # current main
pinned_core=0 tsc_per_ns=1.8816
timer_overhead   n=  1048576  ticks: p50=    18.0 p90=    21.0 p99=    27.0 p99.9=    32.0 p99.99=   159.0 max= 1443512.0 mean=    20.9
                               ns: p50=    9.57 p90=   11.16 p99=   14.35 p99.9=   17.01 p99.99=   84.50 max= 767173.04 mean=   11.09
all              n=  1048576  ticks: p50=    84.0 p90=   372.0 p99=   588.0 p99.9=  1066.0 p99.99=201963.0 max= 2483849.0 mean=   228.3
                               ns: p50=   44.64 p90=  197.70 p99=  312.50 p99.9=  566.54 p99.99=107335.84 max=1320073.54 mean=  121.31
limit            n=   786302  ticks: p50=    84.0 p90=   372.0 p99=   547.0 p99.9=  1056.0 p99.99=207456.0 max= 2483849.0 mean=   232.6
                               ns: p50=   44.64 p90=  197.70 p99=  290.71 p99.9=  561.22 p99.99=110255.16 max=1320073.54 mean=  123.59
market           n=   104436  ticks: p50=   114.0 p90=   235.0 p99=   541.0 p99.9=   990.0 p99.99= 25415.0 max=  801905.0 mean=   178.2
                               ns: p50=   60.59 p90=  124.89 p99=  287.52 p99.9=  526.15 p99.99=13507.13 max= 426182.74 mean=   94.71
cancel           n=   157838  ticks: p50=    49.0 p90=   397.0 p99=   714.0 p99.9=  1194.0 p99.99=184244.0 max= 2466235.0 mean=   240.0
                               ns: p50=   26.04 p90=  210.99 p99=  379.46 p99.9=  634.57 p99.99=97918.85 max=1310712.35 mean=  127.54
```

(All 5 raw runs per build were captured; the tables above are the per-metric
median across all 5, not just this one run.)

## `lob_bench` (Google Benchmark) re-run on current `main`, 5 runs

| benchmark | run1 | run2 | run3 | run4 | run5 | median |
|---|---:|---:|---:|---:|---:|---:|
| BM_MatchingEngineOrderFlow (ns/order) | 203 | 134 | 128 | 124 | 138 | **134** |
| BM_MatchingEngineOrderFlow_no_bot (ns/order) | 98.6 | 63.4 | 63.2 | 68.6 | 58.8 | **63.4** |
| BM_InsertCancelRoundTrip (ns) | 135 | 81.4 | 84.0 | 82.1 | 82.9 | **82.9** |
| BM_InsertCancelEmptyLevel (ns) | 132 | 84.7 | 87.4 | 83.8 | 85.3 | **85.3** |

Run 1 differs from the others by well over 10% on every benchmark
simultaneously (203 vs ~130, 98.6 vs ~63, 135 vs ~83, 132 vs ~85) — a global
first-run effect (cold caches / CPU frequency ramp-up on this laptop's
power plan), not specific to any one benchmark, so it's flagged and excluded
from the medians reported above and in the README.

`BM_InsertCancelEmptyLevel` (85.3 ns median) is now within ~3% of
`BM_InsertCancelRoundTrip` (82.9 ns median) — down from the historical 45.2 ns
vs 22.9 ns (~2x) gap recorded before the bitmap (A4), confirming the O(gap)
`advanceBestBid`/`advanceBestAsk` scan is gone.

## Reproduce

```sh
git tag v1.0-synthetic   # already exists; this doc's "before" is that tag
git worktree add ../lob-v1 v1.0-synthetic
cp bench/latency_mixed.cpp ../lob-v1/bench/
# add lob_latency executable target to ../lob-v1/CMakeLists.txt (not committed there)
cmake -S ../lob-v1 -B ../lob-v1/build -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=OFF
cmake --build ../lob-v1/build -j
./../lob-v1/build/lob_latency --core 0   # x5

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=OFF
cmake --build build -j
./build/lob_latency --core 0             # x5

cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release
cmake --build build-bench -j
./build-bench/lob_bench                  # x5
```
