[![CI](https://github.com/yashshelar466/High-Frequency-Limit-Order-Book/actions/workflows/ci.yml/badge.svg)](https://github.com/yashshelar466/High-Frequency-Limit-Order-Book/actions/workflows/ci.yml)

# High-Frequency Limit Order Book

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A single-threaded limit order book matching engine built in C++17, designed for low-latency execution and deterministic backtesting. Implements price-time priority matching over ordered price maps, a custom memory pool allocator, LIMIT/IOC/FOK/MARKET order types, L2 depth snapshots, and a CSV-driven market data replayer for simulating tick-level trading strategies.

## Table of Contents
- [Overview](#overview)
- [Performance](#performance)
- [Design & Architecture](#design--architecture)
- [Key Optimizations](#key-optimizations)
- [Order Types](#order-types)
- [L2 Depth Snapshots](#l2-depth-snapshots)
- [Trade & Execution Reporting](#trade--execution-reporting)
- [Testing](#testing)
- [Market Data Feed & Replayer](#market-data-feed--replayer)
- [Project Structure](#project-structure)
- [Quick Start](#quick-start)

## Overview

This project implements the core matching logic used in real exchange systems: orders are matched by **price priority**, then **time priority** (FIFO within a price level). It supports the standard order operations — **insert**, **cancel**, and **amend/cancel-replace** (an in-place quantity reduction keeps queue priority; a price change or size increase re-enters the order as a fresh aggressor). The engine is single-threaded by design — this removes lock contention and makes execution fully deterministic, which matters for reproducible backtesting and for reasoning precisely about worst-case latency.

## Performance

Latency is measured with `-O3` and reported **across book depths**, since level lookup is `O(log L)` in the number of live price levels `L` — a single narrow band would flatter the numbers. Resting inserts and cancels are reported separately because they exercise different code paths.

> **Timer-resolution caveat.** On this Windows machine `QueryPerformanceCounter` ticks at ~100 ns, so every figure below is quantized to 100 ns, and the empty-bracket timer overhead (p50) measures as 0 ns — i.e. below a single clock tick. Treat these as ~100 ns-resolution measurements: the **trend across depth** is the meaningful signal, not the absolute value of any one bucket. On Linux the benchmark falls back to `std::chrono::steady_clock` (true nanosecond resolution).

**Resting-insert latency vs. book depth** (one-sided book, no crossing; n = 50,000 per row)

| Live price levels (L) | p50    | p99      |
| --------------------- | ------ | -------- |
| 21                    | 200 ns | 400 ns   |
| 1,000                 | 200 ns | 500 ns   |
| 20,000                | 400 ns | 1,000 ns |

p50 climbing from 200 ns at 21 levels to 400 ns at 20,000 is the `O(log L)` cost of the ordered-map lookup made visible (21 and 1,000 tie only because their ~6-comparison difference is under the 100 ns timer tick). The earlier fixed-array design was `O(1)` here — a deliberate trade for an uncapped price range and cheap L2 depth (see [Design & Architecture](#design--architecture)).

**Cancels** (intrusive-list unlink at a random queue position; n = 80,000)

| Percentile | Latency      |
| ---------- | ------------ |
| avg        | 546 ns       |
| p50        | 400 ns       |
| p90        | 600 ns       |
| p99        | 900 ns       |
| p99.9      | 2,300 ns     |
| max        | 2,511,400 ns |

> The `max` is ~1000× the p99.9 — OS scheduler jitter (context switches, page faults), not the engine. The p99.9 column is the more representative worst case for the algorithm itself.

**Test Environment:** Windows 11, Intel Core i5-8365U @ 1.60GHz, GCC 6.3.0 (MinGW), `-O3`, `QueryPerformanceCounter` (~100 ns resolution).
> Latency numbers are meaningless without hardware context — always report the machine a benchmark ran on.

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

**Pooled order allocation.** A custom `MemoryPool<Order>` serves `Order` objects from a pre-allocated block via placement `new`, keeping order construction off the general heap. This pools *orders only* — each resting order still allocates an `unordered_map` node (the ID index), and each new price level allocates a `PriceLevel` plus a `std::map` node. Measured on the benchmark workload that's roughly **1.1 `new` calls per order**: the pool removes the per-order `Order` allocation and its jitter, not all allocation.

**Ordered price maps.** Bids and asks are ordered maps, so top-of-book is `begin()` (no scanning), depleted levels drop out cleanly, and L2 depth snapshots are a direct ordered walk. Level lookup is `O(log L)` in the number of live price levels — a deliberate trade against the previous fixed-array `O(1)` in exchange for an uncapped price range and correct, cheap depth queries.

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

Trades execute at the resting maker's price (price-time priority), so a single crossing insert can emit several `Trade`s — one per resting order it consumes. This is the hook for a trade blotter, PnL/VWAP, or reconciling against an exchange's own execution feed. When no handler is set, the hot path pays only a single null check per fill. The replayer (`src/replay_main.cpp`) uses it to print a blotter and compute VWAP over a tick file.

## Testing

Two layers of tests guard the matching engine:

- **Unit tests** (`tests/test_matching.cpp`) cover specific behaviors: partial fills, full level sweeps, FIFO preservation after a partial fill, cancellation, invalid inputs, duplicate-order-ID rejection, and top-of-book reset after a level is emptied.
- **Randomized differential test** (`tests/test_differential.cpp`) fires 300k random operations — LIMIT / IOC / FOK / MARKET inserts, cancels, amends, duplicate-ID attempts, and crossings — at both the production engine and a deliberately simple `std::map`-based reference model, asserting they agree on top-of-book and the exact trade stream on every operation, and on full per-level FIFO/volume state periodically. Any divergence points straight at a bug in the fast path — this is what catches whole classes of matching-engine bugs (stale best-price tracking, duplicate-ID handling, misattributed fills) that example-based tests tend to miss.

CI additionally rebuilds and runs both suites under AddressSanitizer + UndefinedBehaviorSanitizer, since the hot path mixes a custom memory pool with raw allocation.

## Market Data Feed & Replayer

An event-driven `MarketDataFeed` parser streams tick data into the engine:
- Processes `ADD`, `CANCEL`, and `EXECUTE` events from CSV input
- Supports timestamp-based playback for backtesting trading strategies against historical or synthetic tick data

## Project Structure

```
Limit-Order-Book/
├── include/              # Public headers (OrderBook, MemoryPool, Order)
├── src/
│   ├── OrderBook.cpp      # Core matching engine
│   └── replay_main.cpp    # Market data replayer entry point
├── tests/
│   ├── test_matching.cpp     # Unit tests for matching logic
│   └── test_differential.cpp # Randomized differential test vs. reference model
├── benchmarks/
│   └── benchmark_latency.cpp
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

# Run the test suites (unit + randomized differential) via CTest
ctest --test-dir build --output-on-failure

# Run the individual binaries
./build/run_benchmark    # latency & throughput benchmark
./build/run_feed         # market data feed replayer (reads data/ticks.csv)
./build/demo             # minimal usage example
```

To build and test with sanitizers (as CI does):

```bash
cmake -S . -B build-san -DENABLE_SANITIZERS=ON
cmake --build build-san -j
ctest --test-dir build-san --output-on-failure
```

### Build & Run (direct g++)

No CMake? Each target is a single translation unit plus `src/OrderBook.cpp`:

```bash
g++ -std=c++17 -Iinclude src/OrderBook.cpp tests/test_matching.cpp -o run_tests && ./run_tests
g++ -O2 -std=c++17 -Iinclude src/OrderBook.cpp tests/test_differential.cpp -o run_diff && ./run_diff
g++ -O3 -std=c++17 -Iinclude src/OrderBook.cpp benchmarks/benchmark_latency.cpp -o run_benchmark && ./run_benchmark
g++ -std=c++17 -Iinclude src/OrderBook.cpp src/replay_main.cpp -o run_feed && ./run_feed
```

## License

Distributed under the MIT License. See `LICENSE` for more information.