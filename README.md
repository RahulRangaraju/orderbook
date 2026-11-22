# orderbook

A C++20 limit order book that matches orders by price-time priority. The flat-array v2 book handles **~19M orders/sec on one core with a p99 of ~550 ns**: 4× the throughput and ~4.4× lower p99 than the `std::map` v1 it replaced. It's checked against v1 by a differential test over 1M+ commands.

![CI](https://github.com/<your-github-username>/orderbook/actions/workflows/ci.yml/badge.svg)

## Build and run

```bash
cmake -S . -B build && cmake --build build -j       # Release by default
ctest --test-dir build --output-on-failure          # 99 test runs
./build/replay data/sample.csv                      # prints trades
```

More:

```bash
./build/gen 1000000 > data/gen_1m.csv               # synthetic flow, fixed seed
./build/replay data/gen_1m.csv --book v2 --quiet    # summary only
./build/replay data/gen_1m.csv --book v2 --threads --queue cached
./build/replay data/mbo.csv --format databento --book v2
./build/bench 5000000                               # throughput + latency, every book
```

Requires CMake ≥ 3.20 and GCC ≥ 11 or Clang ≥ 14. GoogleTest is fetched automatically.

## Design

### Input and output

Every input is a `Command` (`Add`, `Cancel`, `Modify`); every output is a `Trade{buyId, sellId, price, qty}`. Prices are **integer ticks** (cents): `0.1 + 0.2 != 0.3` in floating point, and a book that compares prices must compare them exactly.

Matching rules:
- A buy at P trades against asks while `bestAsk <= P` (a sell: bids while `bestBid >= P`).
- Best price first, then oldest order at that price (FIFO by arrival `seq`).
- Every trade prints at the **resting** order's price. Leftover quantity rests at P.
- Modify: same price and lower quantity keeps queue position. A new price or a larger quantity is cancel + add, which sends the order to the back of the queue (the same rule real exchanges use).

### v1: `OrderBook` (`std::map`)

| Part | Structure |
|---|---|
| Bids | `std::map<Price, std::deque<Order>, std::greater<Price>>` |
| Asks | `std::map<Price, std::deque<Order>>` |
| Cancel index | `std::unordered_map<OrderId, {side, price}>`, then a linear search of that one level |

This version is simple and obviously correct, which makes it the reference v2 is tested against.

### v2: `FlatOrderBook` (flat arrays)

| Part | Structure | Why it's faster |
|---|---|---|
| Price levels | `std::vector<Level>` indexed by `price - minTick` | A lookup is one array access instead of a red-black-tree walk through scattered nodes |
| Orders | `std::vector<Node>` pool with a free list; `prev`/`next` indexes form an intrusive FIFO per level | No `new`/`delete` per order; nodes are contiguous |
| Cancel | id → pool slot, then unlink in O(1) | No queue search |
| Best price | Tracked index plus a bitmap of non-empty levels | When a level empties, `countl_zero`/`countr_zero` find the next level 64 levels at a time |
| Id index | `IdIndex`: a dense `vector<uint32_t>` for ids < 2²⁵, with an `unordered_map` fallback above that | No hash-node allocation per order (see Profiling) |

| Operation | v1 | v2 |
|---|---|---|
| Add (no cross) | O(log L) | O(1) |
| Cancel | O(log L + q) | O(1) |
| Match one fill | O(1) amortized, plus O(log L) per emptied level | O(1), plus a bitmap scan per emptied level |
| Best bid/ask | O(1) | O(1) |

L = price levels on that side, q = orders at the level.

**Prices outside the window:** v2 needs a price window up front. `replay` scans the file and uses the low −10% to high +10%. Orders priced outside the window are **rejected and counted** in `rejected()`, and the run summary reports them. A modify to an out-of-window price leaves the order untouched. The alternative would be a small `std::map` overflow book for outliers: more code on the hot path for a rare case.

## Benchmarks

Machine: Intel Xeon @ 2.10 GHz, 2 vCPUs (KVM cloud VM, no SMT) · GCC 13.3.0 · `Release -O3 -DNDEBUG` · Linux.

Method: 5M synthetic commands (seed 42: 70% add, 25% cancel, 5% modify; 3.1M trades; about 214k orders resting at the end) are generated into a `std::vector<Command>` before timing. Each figure comes from one warm-up pass, then 5 timed runs, and the median is reported. Latency comes from timing every command on its own with `steady_clock` and taking percentiles with `std::nth_element`. Each latency includes about 26 ns of timer overhead, measured and printed by `bench`. Raw output is in [`bench_results/`](bench_results/).

| Version | Orders/sec | p50 latency | p99 latency | p99.9 latency |
|---|---|---|---|---|
| v1 (`std::map`) | 4.75M | 181 ns | 2.41 µs | 7.93 µs |
| v2 (flat array, `unordered_map` id index) | 8.0–8.8M | 113–121 ns | 1.11–1.19 µs | 2.2–2.3 µs |
| **v2 (flat array, dense id index)** | **19.0M** | **73 ns** | **547 ns** | **1.01 µs** |
| v2 + SPSC threads, end to end (parse + match) | 6.6–14.3M | 620–700 ns* | 20–280 µs* | — |

\* The pipeline was measured paced at 1M commands/sec, so the queue stays nearly empty. At full speed the queue fills and latency becomes backlog time (14–33 ms), which measures queueing rather than the queue hop; see Threading. The p99 varied 14× between runs, most likely from VM scheduling noise.

The pipeline row is not comparable to the rows above it: it includes CSV parsing, and the rows above don't. Its direct comparison is one thread doing parse + match, which measured 8.2–10.1M/sec on the same machine. Ranges are spreads across repeated invocations, since 2-vCPU cloud VMs are noisy. **Numbers depend on the machine; re-run `./build/bench 5000000` on yours.**

## Threading

`SpscQueue<T, N>` is a lock-free single-producer/single-consumer ring buffer. Thread 1 parses CSV lines and pushes `Command`s; thread 2 pops them and runs the book. Each thread is pinned to its own core with `pthread_setaffinity_np`.

- **Why it's lock-free:** only the producer writes `tail_` and only the consumer writes `head_`. Each side reads the other's index with `acquire` and publishes its own with `release`.
- **Memory ordering:** the producer writes the slot, then does `tail_.store(release)`. The consumer's `tail_.load(acquire)` therefore guarantees it sees the slot's contents. As an experiment, I set every ordering to `relaxed` and ran the pipeline test under ThreadSanitizer. It reported data races on the buffer slots immediately.
- **False sharing:** `alignas(64)` puts `head_`, `tail_` and the buffer on separate cache lines. Without it, every push invalidates the consumer's copy of `head_`'s line, and the reverse.
- **Power-of-2 size:** `& (N - 1)` replaces `%`.
- **Busy-wait:** an empty or full queue spins with `pause`. That gives the lowest latency, but it keeps **two cores at 100%** for the whole run. A production system would add a back-off or sleep for quiet periods.
- **Shutdown:** the producer sets an atomic `done` flag (release) after its last push. The consumer drains the queue after seeing it (acquire), so nothing is lost.

**What benchmarking found.** With the guide's queue, two threads were *slower* than one (5.1–7.3M/sec vs 8.2–10.1M/sec). Every `push` reads `head_` and every `pop` reads `tail_`, and the other core is constantly writing those cache lines, so nearly every operation pays a cross-core miss. `SpscQueueCached` gives each side a private copy of the other's index and re-reads the shared atomic only when the queue *looks* full or empty. It beat the guide's queue in all 7 paired runs (1.2–2.0×). On this 2-vCPU VM, two threads ended up between 0.7× and 1.5× of one thread depending on noise. A dedicated machine should do better. Both queues are kept, tested and benchmarked side by side.

**The trade-off:** pipelining can raise throughput (at best to 1 / max(parse, match) instead of 1 / (parse + match)), but each command pays a queue hop, about 0.6 µs median on this VM. When the producer outruns the consumer, the queue fills and latency turns into time spent waiting in the backlog.

## Profiling (Step 9)

`perf` isn't available in the cloud VM I developed on, so I used Valgrind's **callgrind** with cache simulation (`--cache-sim=yes`) on 500k commands. On a Linux machine, use the commands at the end of this section. Full output: [`bench_results/profile_before_callgrind.txt`](bench_results/profile_before_callgrind.txt) and [`profile_after_callgrind.txt`](bench_results/profile_after_callgrind.txt).

**What it showed:** in v2, `std::unordered_map` operations (`removeOrder`, `cancel`, `add`, `rest`) accounted for about **30% of all L1 data-cache misses**, and `malloc`/`free` for about 14% of instructions. Every insert allocates a hash node, and every lookup follows a pointer to a cold one. That matches the "malloc/free" and "hash lookups" rows in the guide's hot-spot table.

**What I changed:** I added `IdIndex`, a dense `vector<uint32_t>` indexed by id, with an `unordered_map` fallback for ids ≥ 2²⁵ such as 64-bit exchange order ids. Behaviour is unchanged: the differential tests, including one with exchange-style 64-bit ids that exercises both paths, still pass.

**Result:** v2 went from **8.0–8.8M to 19.0M orders/sec** and **p99 from ~1.15 µs to 547 ns**. Whole-program instructions fell 26% and L1 data misses fell 31%. `malloc` and the hashtable functions dropped out of the profile entirely.

A second, smaller finding: the API that returns a fresh `std::vector<Trade>` per order runs at 11.9M/sec, against 19.0M/sec for appending into one reused buffer (`./build/bench 5000000 --book v2 --api vector`). The hot path uses the reused buffer, and the vector-returning API is kept for tests.

Leak check: `valgrind --leak-check=full` on `replay` (v1, v2, threaded and Databento modes) reports 0 bytes in use at exit and 0 errors ([`bench_results/valgrind_leak_check.txt`](bench_results/valgrind_leak_check.txt)).

```bash
# On a Linux machine with perf:
cmake -S . -B build-prof -DOB_PROFILE=ON && cmake --build build-prof -j
perf stat -e cycles,instructions,cache-misses,branch-misses ./build-prof/bench 5000000 --book v2
perf record -g ./build-prof/bench 5000000 --book v2 && perf report
```

## Testing

There are **65 test cases (99 runs)**: the 34 order-book tests are `TYPED_TEST`s that run against both v1 and v2.

- **Book behaviour:** empty book, resting, exact and partial fills, the Step 3 worked example, sweeps, time priority, trade price, every cancel edge case, modify (keeps or loses priority, crossing, unknown id, zero quantity), reduce and clear, sell-side mirrors of each, and v2 window and bitmap edge cases.
- **Randomized invariants:** 100k commands per run, with `validate()` after every command. Checks: never crossed, no empty levels, index consistency, FIFO `seq` order, level totals.
- **Differential:** 1M generated commands plus 300k "chaos" commands (duplicate ids, unknown cancels, zero-quantity modifies) plus 200k with 64-bit ids. v1 and v2 must produce identical trades and top of book after every command.
- **Parsers:** our CSV format and the Databento MBO format (column lookup by header name, fixed-point and decimal prices, partial cancels, clear).
- **Concurrency:** 10M sequential integers through each queue must arrive exactly once and in order, and the threaded replay must produce the same trades as single-threaded. CI runs these under ThreadSanitizer.
- **Mutation check:** I broke the code on purpose five times. Four bugs (wrong trade price, lost modify priority, empty levels left behind, wrong level totals) failed 3–11 tests each. The fifth (best price not advanced) made matching loop forever, which the per-test timeout catches.

CI (GitHub Actions) builds and tests with GCC and Clang, replays the sample, and runs the threaded tests under TSan. Locally, the suite is also clean under ASan + UBSan (`-DOB_SANITIZE=address,undefined`).

## Databento market-by-order data

`replay --format databento` reads Databento's MBO CSV. Columns are found by header name (`action`, `side`, `price`, `size`, `order_id`), so column order doesn't matter.

| Databento | Engine |
|---|---|
| `A` add | `add` (side `B` → Buy, `A` (ask) → Sell) |
| `C` cancel | `reduce(id, size)`: **cancels can be partial**; a full cancel removes the order |
| `M` modify | `modify` (an unknown id is treated as an add, e.g. orders placed before the file starts) |
| `R` clear | `clear()` |
| `T` / `F` / `N` | ignored: fills on the book always arrive as cancels too, and the engine produces its own trades |
| `price` | fixed-point integer (1e-9 dollars) or a decimal string → ticks via `--tick-nanos` (default 10,000,000 = 1 cent) |

`data/mbo_format_example.csv` is a **hand-written** 11-line file in Databento's column layout (symbol `DEMO`, invented prices) for testing the parser. It is not real market data. To run a real day, download one symbol's MBO data as CSV from your Databento account, then `./build/replay mbo.csv --format databento --book v2 --quiet`. The final best bid/ask should look like real prices for that stock. Data for some venues needs extra handling (for example, the snapshot records at the start of each day).

## Known limits

- One symbol per book (multi-symbol: one book per symbol, sharded across threads).
- Limit orders only: no market, IOC or iceberg orders. No self-trade prevention.
- v2 needs a price window up front, and prices outside it are rejected.
- The two-thread pipeline busy-spins two cores.
- Benchmarks come from a 2-vCPU cloud VM, so the multi-thread numbers in particular are noisy.

## Layout

```
include/ob/   types.hpp  order_book.hpp (v1)  flat_order_book.hpp (v2)  id_index.hpp
              spsc_queue.hpp  pipeline.hpp  parser.hpp  generator.hpp  engine.hpp
src/          order_book.cpp  flat_order_book.cpp
apps/         replay.cpp  gen.cpp  bench.cpp
tests/        test_order_book.cpp  test_randomized.cpp  test_parser.cpp  test_spsc.cpp
data/         sample.csv  mbo_format_example.csv   (big files stay out of git)
bench_results/ raw benchmark, profile and leak-check output
docs/         interview-prep.md
```
