# High-Frequency Limit Order Book

[![CI](https://github.com/yashshelar466/High-Frequency-Limit-Order-Book/actions/workflows/ci.yml/badge.svg)](https://github.com/yashshelar466/High-Frequency-Limit-Order-Book/actions/workflows/ci.yml)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A single-threaded limit order book matching engine in C++17, built to be **correct first and measured honestly second**. Price-time priority matching over ordered price maps, a pooled order allocator, LIMIT/IOC/FOK/MARKET order types, L2 depth snapshots, and a market-data replayer that reconciles the reconstructed book against a real exchange's own published book, message by message.

The claim this project makes is not "fastest." It is: *the matching logic is verified against a reference model and against real NASDAQ data, and every latency number here comes with the machine it was measured on and the benchmark that produced it.*

## Table of Contents
- [Overview](#overview)
- [Correctness](#correctness)
- [Real Market-Data Reconciliation (LOBSTER)](#real-market-data-reconciliation-lobster)
- [Performance](#performance)
- [Design & Architecture](#design--architecture)
- [Key Optimizations](#key-optimizations)
- [Order Types](#order-types)
- [L2 Depth Snapshots](#l2-depth-snapshots)
- [Trade & Execution Reporting](#trade--execution-reporting)
- [Market Data Feed & Replayer](#market-data-feed--replayer)
- [Structure](#structure)
- [Quick Start](#quick-start)

## Overview

This matching engine implements the core matching logic used in real exchange systems: orders are matched by **price priority**, then **time priority** (FIFO within a price level). It supports the standard order operations — **insert**, **cancel**, and **amend/cancel-replace** (an in-place quantity reduction keeps queue priority; a price change or size increase re-enters the order as a fresh aggressor). The engine is single-threaded by design — this removes lock contention and makes execution fully deterministic, which matters for reproducible backtesting and for reasoning precisely about worst-case latency.

## Correctness

A matching engine that is fast and wrong is worthless, so this is the part of the project I'd defend first. Four suites guard the engine, and two of them check it against something other than my own expectations — a reference implementation, and a real venue's published book.

- **Unit tests** (`tests/test_matching.cpp`) cover specific behaviors: partial fills, full level sweeps, FIFO preservation after a partial fill, cancellation, invalid inputs, duplicate-order-ID rejection, top-of-book reset after a level is emptied, amend/cancel-replace priority semantics, IOC/FOK/MARKET handling, and L2 depth snapshots (including empty and one-sided books).
- **Randomized differential test** (`tests/test_differential.cpp`) fires 300k random operations — LIMIT / IOC / FOK / MARKET inserts, cancels, amends, duplicate-ID attempts, and crossings — at both the production engine and a deliberately simple `std::map`-based reference model, checking they agree on top-of-book and the exact trade stream on every operation, and on full per-level FIFO/volume state periodically. Any divergence points straight at a bug in the fast path — this is what catches whole classes of matching-engine bugs (stale best-price tracking, duplicate-ID handling, misattributed fills) that example-based tests tend to miss.
- **LOBSTER replay test** (`tests/test_lobster_replay.cpp`) drives the market-data reconciliation logic with synthetic fixtures in LOBSTER's exact CSV format, since the real data isn't redistributed here. It checks that a correct stream reconciles cleanly, that the warm-start seeding correctly bootstraps pre-existing opening liquidity the message stream never explains, and — importantly — that a deliberately corrupted published book and a dropped message are **detected**. A reconciler that silently passed everything would otherwise be indistinguishable from one that works.
- **Allocator lifetime test** (`tests/test_memorypool.cpp`) exercises `MemoryPool<T>` with a non-trivially-destructible `T`. The pool is backed by raw uninitialized storage and destroys each object exactly once (on `deallocate`), so its own teardown can't double-destroy a slot — a bug that stays invisible with a trivially-destructible type like `Order` and only surfaces under a type that owns a heap buffer. It also drives a deliberately tiny pool past capacity to cover the **overflow path**: `deallocate` skips its heap-pointer lookup entirely unless the pool has ever spilled (see [Key Optimizations](#key-optimizations)), and that shortcut is only safe if a heap pointer can never be mistaken for a pooled slot — which would placement-destroy it and file it onto the free list as pool storage. The test releases pooled and heap objects interleaved and asserts each went down the route it came from; disabling the fast-path guard turns it red immediately.

**Checks are never compiled out.** All assertions go through a `CHECK` macro (`tests/check.hpp`) that prints the failed expression and exits non-zero, rather than `assert`, which expands to nothing whenever `NDEBUG` is defined — as it is in any optimized Release build. CI therefore runs the suites in **Debug and Release**, plus a third pass under **AddressSanitizer + UndefinedBehaviorSanitizer** (explicitly a Debug build, so the sanitizers aren't weakened by `-O3`/`NDEBUG`). A red CI job means a real failure; injecting a deliberate bug into the matching path turns the suites red in every configuration.

## Real Market-Data Reconciliation (LOBSTER)

The randomized differential test checks the engine against a reference model I wrote. The stronger test is checking it against **a real exchange's own book**.

[LOBSTER](https://lobsterdata.com/info/DataSamples.php) publishes two aligned CSVs per ticker-day: a **message file** (every event — new limit orders, partial cancels, deletions, visible and hidden executions) and an **orderbook file** whose row *k* is the venue's published top-N book *after* message *k*. `run_lobster` replays the message stream through the engine and compares the reconstructed book against the published one after **every single message**, exiting non-zero on the first divergence.

```bash
# Download a free sample day from lobsterdata.com into data/ (not redistributed here)
# Strict: report the exact reconstruction horizon
./build/run_lobster data/AAPL_2012-06-21_34200000_57600000_message_10.csv \
                    data/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv 10

# Recover: continue past unexplained levels, counting each adoption
./build/run_lobster data/AAPL_2012-06-21_34200000_57600000_message_10.csv \
                    data/AAPL_2012-06-21_34200000_57600000_orderbook_10.csv 10 --recover
```

How the events map onto the engine (per LOBSTER's documented message types — this format version documents 1, 2, 3, 4, 5, 7; type 6 is handled defensively in case a newer LOBSTER release emits it, though it never appears in the sample used here):

| LOBSTER event | Applied as |
| ------------- | ---------- |
| 1 — new limit order | `insert_order` (the stream is already matched, so it rests) |
| 2 — partial cancel | `reduce_order` (shrink in place, keeping queue priority) |
| 3 — full delete | `cancel_order` |
| 4 — visible execution | `reduce_order` — the venue already matched it |
| 5 — hidden execution | skipped; hidden liquidity is not on the visible book |
| 7 — trading halt indicator | skipped (LOBSTER duplicates the preceding book row during a halt, so there's nothing to apply) |

Two details worth calling out. First, executions are applied as **reductions rather than by letting the engine match** — LOBSTER's stream is post-match, so re-matching it would double-count. The replayer registers a trade handler that should therefore *never* fire; if it does, the book has drifted into a crossed state and the run fails loudly. Second, event types 2 and 4 both express a *delta* ("this order shrank by N"), which is why the engine has `reduce_order` alongside `amend_order`'s absolute-size semantics — real feeds publish deltas.

**The book isn't empty when the message stream starts, and the reconciler accounts for that.** Replaying an actual NASDAQ session (AAPL, 2012-06-21) against its own published book failed immediately at message 1: the venue's first row already showed a fully populated ask ladder and several bid levels beyond message 1's own order — real resting liquidity established by the opening cross, with no "new order" message for it anywhere in the file, since the capture window begins exactly at market open. Both LOBSTER's own reconstruction and a naive from-empty replay hit this. The fix: infer the implied state strictly *before* message 1 by reversing its own contribution out of the published first row, then seed everything that's left — pre-existing liquidity with no message provenance in this file — as resting orders under synthetic order IDs clearly out of range of any real LOBSTER ID. Message 1 is then applied for real through the normal path, and the seeded book matches the published row exactly. This is validated by a dedicated test (`tests/test_lobster_replay.cpp`) that reproduces the shape at small scale, independent of the real data.

**Later messages can still reference that seeded liquidity, and it's handled.** Replaying further surfaced the next problem: a delete at message 58 named order id `15836282` — far older than any id in the file, i.e. an order resting before the window and therefore seeded under a synthetic id. The id lookup finds nothing, so the reduction was being dropped and the level drifted 100 shares high. The fix is to fall back on what the venue *is* telling us: an unresolvable delete or execution at a (side, price) where we hold seeded liquidity is attributed to that seeded aggregate. Order-level identity is unrecoverable, but the aggregate book — the thing an L2 reconciliation actually compares — stays correct. Runs report this as `events attributed to seeded pre-window liquidity`, distinct from genuinely unresolvable `unknown_refs`.

**Two ways to read the result: strict horizon, or recover-and-count.** Because a top-N feed is a windowed view rather than a complete event stream (see below), perfect from-scratch reconstruction of a full session is impossible *in principle* from level-10 data. `run_lobster` therefore supports both honest readings:

- **Strict (default)** — stop at the first published level the message stream cannot explain, and report how far the book reconstructed *exactly*. This measures the reconstruction horizon.
- **`--recover`** — resynchronize structure against the published book the way a production feed handler treats a detected gap, and report how much resynchronizing the session required. It works in both directions: **adopting** levels the venue reports that we never saw, and **pruning** levels we hold that the venue does not (liquidity can die below the window just as silently as it can appear). Both are counted separately. What it does *not* forgive: a **size mismatch** on a level both sides track — that is the engine's own arithmetic, and it still fails the run. A test asserts exactly that, so "recover" can never quietly degrade into "never report anything." Levels that would cross the book are also left alone, since those indicate a genuine phantom on the opposite side.

Measured strict horizons on AAPL 2012-06-21, which show the effect clearly:

| Compare depth | Messages reconciled exactly |
| ------------- | --------------------------- |
| 1 (top of book) | 441 |
| 2 | 226 |
| 3 | 160 |
| 5 | 156 |
| 10 | 13 |

Monotonic, and for one reason: every one of those runs ends on the *same* phantom level — order `13419503`, 50 shares at `5854000`, which sits in the venue's published book with no submission message anywhere in the file. The comparison depth only determines how long it takes that level to climb into the window being checked.

**The remaining known limit — the depth window.** LOBSTER emits messages only for events *"in the requested price range"* (its readme). Liquidity that drifts below level N as better prices arrive can then be cancelled or executed **entirely outside the window, generating no message at all**, while a deeper level silently promotes into view in the published book. Tracing rows 12→13 of the AAPL session shows exactly this: `5876500 x 1160` vanishes from the venue's book with no corresponding message, and `5879000 x 500` appears at level 10 from outside the window. No reconstruction can recover events it was never told about, so the **deepest levels are structurally unreliable** and divergence there is expected rather than a bug. Reconciling a shallower window than you ingest (e.g. `run_lobster ... 5` against level-10 data) keeps the comparison inside the region the message stream can actually explain. Note that seeding always uses the file's **full** published depth even when the comparison is narrowed — liquidity below the comparison window still promotes into view as the top is consumed, so discarding it just guarantees a later divergence.

Separately, this reconciles the *visible* book only — hidden executions are excluded by design.

## Performance

Everything below is produced by `benchmarks/benchmark_latency.cpp` (`./build/run_benchmark`) — no figure in this README comes from anywhere else. Latency is measured with `-O3` and reported **across book depths**, since level lookup is `O(log L)` in the number of live price levels `L` and a single narrow band would flatter the numbers. Percentiles are reported rather than averages, because an average hides exactly the tail an exchange cares about.

**Test environment:** Ubuntu 24.04, Intel Xeon @ 2.80 GHz (4 vCPU, shared cloud instance), GCC 13.3.0, `-O3`, `std::chrono::steady_clock`.

> **Read the tails with suspicion.** This is a shared virtual machine, not tuned bare metal — no core pinning, no isolated CPUs, frequency scaling and neighbours both in play. p50 is stable to ±2 ns across runs and is the number to trust; p99.9 and `max` swing by an order of magnitude run to run and are dominated by scheduler preemption rather than by the engine. Where a comparison matters below, I quote the **difference between two arms measured in the same run**, which cancels both the timer overhead and most of the machine noise.

> **Timer overhead:** the empty-bracket `now_ns()` pair costs **22 ns** at p50 on this machine, and the benchmark prints it first. That 22 ns is included in every absolute figure below — so a reported 107 ns insert is ~85 ns of engine work. It is *not* included in any A/B difference, since both arms pay it.

**Resting-insert latency vs. book depth** (one-sided book, no crossing; n = 50,000 per row)

| Live price levels (L) | p50    | p99      |
| --------------------- | ------ | -------- |
| 21                    | 107 ns | 2,672 ns |
| 1,000                 | 152 ns | 2,493 ns |
| 20,000                | 328 ns | 2,964 ns |

p50 climbing 107 → 152 → 328 ns as the book grows from 21 to 20,000 live levels is the `O(log L)` ordered-map lookup made directly visible — a tripling of median latency across three orders of magnitude of depth, which is what `log L` should look like. This is a real cost of the design, not a rounding error, and it is the honest counterweight to the `std::map` choice: an array-indexed book over a bounded price range would be `O(1)` here (see [Design & Architecture](#design--architecture)). The p99 column shows no clean trend, for the reason in the caveat above — at that percentile this machine's noise floor is larger than the effect being measured.

**Cancels** (intrusive-list unlink at a random queue position; n = 80,000)

| Percentile | Latency    |
| ---------- | ---------- |
| avg        | 268.989 ns |
| p50        | 226 ns     |
| p90        | 371 ns     |
| p99        | 614 ns     |
| p99.9      | 3,269 ns   |
| max        | 57,663 ns  |

Cancel is an `O(1)` unlink, so the work here is the `unordered_map` id lookup plus pointer surgery; the `max` at ~18× the p99.9 is the VM, not the algorithm.

**Allocator A/B — does the memory pool actually pay for itself?** Both arms run the identical churn pattern (hold 20,000 live `Order`s, then repeatedly release one at a random slot and allocate a replacement, timing the pair), one served by `MemoryPool<Order>` and one by global `new`/`delete`; n = 100,000 each.

| Percentile | `MemoryPool` | `new`/`delete` | Difference |
| ---------- | ------------ | -------------- | ---------- |
| p50        | 31 ns        | 57 ns          | **−26 ns** |
| p90        | 46 ns        | 96 ns          | −50 ns     |
| p99        | 72 ns        | 184 ns         | −112 ns    |
| p99.9      | 188 ns       | 444 ns         | −256 ns    |

The pool saves ~26 ns per allocate/free pair at the median, and the gap widens sharply into the tail (−112 ns at p99, −256 ns at p99.9) — which is the more interesting result: a free-list pop has essentially fixed cost, while a general-purpose allocator's worst case includes size-class bookkeeping and occasional trips to the OS. Both arms pay the same 22 ns of timer overhead, so the *differences* in that last column are overhead-free. Net of it, the pool is roughly 9 ns vs. 35 ns of real work at p50. Note the scope of the claim: this measures the `Order` allocation only, and the engine performs ~1.1 `new` calls per order overall (see [Key Optimizations](#key-optimizations)), so this is not a whole-engine speedup.

**Trade-handler dispatch cost.** The reporting hook is a `std::function`, so each fill pays an indirect call the compiler cannot inline. Identical single-fill crossing inserts, handler unset vs. set (n = 50,000 each): p50 **87 ns → 89 ns**, i.e. about **2 ns per fill**, reproducible to ±1 ns across runs. That is the real price of the type-erased handler — small, but it is a cost, and the alternative is discussed in [Trade & Execution Reporting](#trade--execution-reporting).

Reproduce all of the above with `cmake --build build --target run_benchmark && ./build/run_benchmark`. If you run it on your own machine the absolute numbers will differ; the depth trend and the A/B gaps should not.

## Design & Architecture

```
                    ┌─────────────────────┐
                    │     MarketDataFeed    │
                    │   (CSV tick replay)   │
                    └──────────┬───────────┘
                               │ ADD / CANCEL / EXECUTE
                               ▼
                    ┌─────────────────────┐
                    │      OrderBook        │
                    │  price-time priority  │
                    └──────────┬───────────┘
                               │
                 ┌─────────────┴─────────────┐
                 ▼                           ▼
        ┌────────────────┐         ┌────────────────┐
        │   Bid Levels     │         │   Ask Levels     │
        │  std::map desc   │         │  std::map asc    │
        │  price → FIFO    │         │  price → FIFO    │
        │  linked list     │         │  linked list     │
        └────────────────┘         └────────────────┘
                 │                           │
                 └─────────────┬─────────────┘
                               ▼
                    ┌─────────────────────┐
                    │   MemoryPool<Order>   │
                    │  placement-new alloc  │
                    └─────────────────────┘
```

Each side of the book is an ordered `std::map` from price to price level — bids descending, asks ascending — so the best bid/ask is always the first key (`begin()`), and depth walks in price order for free. Each price level is an intrusive doubly-linked list, so cancellation is O(1) (unlink in place, no shifting) and appending a new order at an existing level is O(1); locating or creating the level is `O(log L)` in the number of distinct live price levels `L`. The full `uint32` price range is supported (only `0` and `0xFFFFFFFF` are reserved as empty-side sentinels).

## Key Optimizations

**Pooled order allocation.** A custom `MemoryPool<Order>` serves `Order` objects from a pre-allocated block via placement `new`, keeping order construction off the general heap. Measured against `new`/`delete` on the same churn pattern, this is worth **~26 ns per allocate/free pair at p50 and ~256 ns at p99.9** — the [A/B benchmark](#performance) is in the repo, and the tail is where the win actually lives.

Two limits on that claim, both worth stating plainly. It pools *orders only*: each resting order still allocates an `unordered_map` node (the ID index), and each new price level allocates a `PriceLevel` plus a `std::map` node — roughly **1.1 `new` calls per order** on the benchmark workload. And it is a *fixed-capacity* pool, so it needs an answer for exhaustion: allocation past capacity falls back to the general heap, and `deallocate` has to know which of the two a pointer came from. The obvious implementation of that — look the pointer up in a set of heap allocations on every release — puts a hash and a probe on the cancel and fill paths of every run, including the overwhelming majority that never overflow at all. Instead a sticky `has_overflowed_` flag guards the lookup, so a pool that stays within capacity never consults the set. The overflow itself is recorded and read back through `overflow_total()` rather than logged: the first version wrote to `stderr` from inside `allocate()`, which is a locked, flushing, syscall-backed write per allocation, triggering precisely when the system is already in the trouble that exhausted the pool. Diagnostics belong out of the hot path.

**Ordered price maps.** Bids and asks are ordered maps, so top-of-book is `begin()` (no scanning), depleted levels drop out cleanly, and L2 depth snapshots are a direct ordered walk. Level lookup is `O(log L)` in the number of live price levels — a deliberate trade against the previous fixed-array `O(1)` in exchange for an uncapped price range and correct, cheap depth queries. The [depth-swept benchmark](#performance) prices that trade at 107 → 328 ns p50 across 21 → 20,000 levels; a flat array indexed by price over a bounded tick range is the standard HFT answer and the most obvious next optimization here, at the cost of a hard price ceiling and memory proportional to the range rather than to live levels.

## Order Types

The engine supports the standard time-in-force / order types via an `OrderType` argument to `insert_order` (the 4-argument form defaults to `LIMIT`):

| Type     | Behavior |
| -------- | -------- |
| `LIMIT`  | Match whatever crosses the limit price, then rest any remainder on the book. |
| `IOC`    | Immediate-Or-Cancel: match what crosses now, discard the remainder (never rests). |
| `FOK`    | Fill-Or-Kill: fill the entire quantity immediately, or do nothing at all. |
| `MARKET` | Ignore the price limit and take liquidity until filled or the opposite side is exhausted; never rests. |

```cpp
book.insert_order(1, 105, 30, false);                    // resting LIMIT ask
book.insert_order(2, 105, 50, true, OrderType::IOC);     // fills 30, drops the rest
book.insert_order(3, 0,  40, true, OrderType::MARKET);   // price ignored; sweeps the book
```

`FOK` first checks resting liquidity (an ordered walk of the opposite side) and only proceeds if the full size can be filled, so it never leaves a partial fill behind.

## L2 Depth Snapshots

Because each side of the book is an ordered map, market-by-price depth is a direct top-of-book walk:

```cpp
for (const DepthLevel& lvl : book.get_ask_depth(5))   // best 5 ask levels, low -> high
    std::cout << lvl.price << " x " << lvl.volume << " (" << lvl.order_count << ")\n";

book.get_bid_depth(5);   // best 5 bid levels, high -> low
book.spread();           // best_ask - best_bid (0 if one-sided)
book.mid_price();        // (best_bid + best_ask) / 2 (0 if one-sided)
```

Each `DepthLevel` carries `{ price, volume, order_count }`. The replayer prints a top-5 snapshot plus spread/mid after a run.

## Trade & Execution Reporting

Matching is only half the story — a backtester needs to *observe* the fills the engine produces. Register a trade handler and the engine invokes it synchronously for every fill, in execution order:

```cpp
OrderBook book;
book.set_trade_handler([](const Trade& t) {
    // taker_id  – aggressing (incoming) order
    // maker_id  – resting order that was hit
    // price     – execution price (the resting maker's price)
    // qty       – quantity filled
    // taker_is_buy – side of the aggressor
    std::cout << t.qty << " @ " << t.price << '\n';
});
```

Trades execute at the resting maker's price (price-time priority), so a single crossing insert can emit several `Trade`s — one per resting order it consumes. This is the hook for a trade blotter, PnL/VWAP, or reconciling against an exchange's own execution feed. The replayer (`src/replay_main.cpp`) uses it to print a blotter and compute VWAP over a tick file.

**The dispatch tradeoff, stated honestly.** `TradeHandler` is a `std::function`, which means every fill pays an indirect call through a type-erased target that cannot be inlined. [Measured](#performance): **~2 ns per fill** (p50 87 → 89 ns on a single-fill crossing insert), and nothing at all when no handler is set — that path is one null check. The alternative is to template `OrderBook` on the handler type, which would let the compiler inline the callback and fold away the branch entirely. I've deliberately not done that: it would make the engine header-only, force every translation unit to recompile the matching logic, and mean the engine's type changes with its observer — a steep price for 2 ns on a book whose median insert is ~85 ns and whose depth-scaling costs 200 ns on its own. If this engine were ever colocated and the 2 ns mattered, templating the handler is the fix, and the benchmark to justify it already exists.

## Market Data Feed & Replayer

An event-driven `MarketDataFeed` parser streams tick data into the engine:
- Processes `ADD`, `CANCEL`, and `EXECUTE` events from CSV input
- Supports timestamp-based playback for backtesting trading strategies against historical or synthetic tick data

## Structure

```
Limit-Order-Book/
├── include/                     # Public headers
│   ├── OrderBook.hpp             # Book, Order, PriceLevel, Trade, DepthLevel
│   ├── MemoryPool.hpp            # Pooled allocator over raw storage
│   ├── MarketDataFeed.hpp        # CSV tick parser / replayer
│   └── LobsterReplay.hpp         # LOBSTER replay + book reconciliation
├── src/
│   ├── OrderBook.cpp             # Core matching engine
│   ├── replay_main.cpp           # Market data replayer entry point
│   ├── lobster_replay.cpp        # LOBSTER reconciliation CLI
│   └── main.cpp                  # Minimal usage example
├── tests/
│   ├── check.hpp                 # CHECK macro (survives NDEBUG — see Correctness)
│   ├── test_matching.cpp         # Unit tests for matching logic
│   ├── test_differential.cpp     # Randomized differential test vs. reference model
│   ├── test_lobster_replay.cpp   # LOBSTER reconciliation (synthetic fixtures)
│   └── test_memorypool.cpp       # Allocator lifetime + overflow-path test
├── benchmarks/
│   └── benchmark_latency.cpp     # Depth sweep, cancels, allocator A/B, handler cost
├── data/
│   └── ticks.csv                 # Sample tick data for the replayer
├── .github/workflows/ci.yml      # Debug + Release + sanitizer CI
├── CMakeLists.txt
└── README.md
```

## Quick Start

### Prerequisites
- C++17-compatible compiler (g++ or Clang)
- CMake 3.14+
- Git

### Build & Run (CMake)

```bash
# Clone the repository
git clone https://github.com/yashshelar466/High-Frequency-Limit-Order-Book.git
cd High-Frequency-Limit-Order-Book

# Configure and build everything (Release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Run the test suites (unit + differential + allocator + LOBSTER) via CTest
ctest --test-dir build --output-on-failure

# Run the individual binaries
./build/run_benchmark    # latency benchmark: depth sweep, cancels, allocator A/B, handler cost
./build/run_feed         # market data feed replayer (reads data/ticks.csv)
./build/demo             # minimal usage example
./build/run_lobster      # LOBSTER reconciliation CLI (needs real data — see above)
```

To build and test with sanitizers (as CI does):

```bash
# Debug is explicit: ASan/UBSan lose coverage under -O3/NDEBUG.
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-san -j
ctest --test-dir build-san --output-on-failure
```

### Build & Run (direct g++)

No CMake? Each target is a single translation unit plus `src/OrderBook.cpp`:

```bash
g++ -std=c++17 -Iinclude src/OrderBook.cpp tests/test_matching.cpp -o run_tests && ./run_tests
g++ -O2 -std=c++17 -Iinclude src/OrderBook.cpp tests/test_differential.cpp -o run_diff && ./run_diff
g++ -std=c++17 tests/test_memorypool.cpp -o run_pool && ./run_pool
g++ -O2 -std=c++17 -Iinclude src/OrderBook.cpp tests/test_lobster_replay.cpp -o run_lobster_test && ./run_lobster_test
g++ -O3 -std=c++17 -Iinclude src/OrderBook.cpp benchmarks/benchmark_latency.cpp -o run_benchmark && ./run_benchmark
g++ -std=c++17 -Iinclude src/OrderBook.cpp src/replay_main.cpp -o run_feed && ./run_feed
```

## License

Distributed under the MIT License. See `LICENSE` for more information.