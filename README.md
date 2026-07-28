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

Benchmarked on 100,000 randomized order operations (inserts, executions, cancellations) over a 21-tick price band, compiled with `-O3`.

| Metric               | Result                  |
| -------------------- | ------------------------ |
| Throughput           | ~4.5M ops/sec             |
| Total Execution Time | ~22 ms (100k orders)      |

Latency is reported separately for resting inserts (no match) and crossing inserts (matched against the book), since a multi-level matching sweep is a fundamentally different cost than a plain resting insert — a single blended average hides that distinction.

**Resting inserts (no match)** — n=56,332

| Percentile | Latency    |
| ---------- | ---------- |
| avg        | 161 ns     |
| p50        | 126 ns     |
| p90        | 190 ns     |
| p99        | 319 ns     |
| p99.9      | 3,769 ns   |
| max        | 325,425 ns |

**Crossing inserts (matched against book)** — n=43,668

| Percentile | Latency   |
| ---------- | --------- |
| avg        | 166 ns    |
| p50        | 137 ns    |
| p90        | 287 ns    |
| p99        | 456 ns    |
| p99.9      | 743 ns    |
| max        | 62,885 ns |

> The max values are 100-1000x larger than p99.9, which points to OS scheduler jitter (context switches, page faults) rather than the matching engine itself — the p99.9 column is the more representative worst case for the algorithm's actual behavior.

> **Design note:** price levels are held in ordered `std::map`s, so lookup/insert/erase is `O(log L)` in the number of *distinct live price levels* `L` (typically small), not the earlier fixed-array `O(1)`. This trades a modest, measured latency cost (≈40 ns at the median) for an uncapped price range and correct, cheap L2 depth snapshots. The O(1) memory pool is unchanged.

**Test Environment:** Ubuntu 24.04, Intel Xeon @ 2.80GHz, GCC 13.3, `std::chrono::steady_clock`.
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

**Zero-allocation execution.** A custom contiguous `MemoryPool<Order>` uses placement `new` to pre-allocate order objects, eliminating `malloc`/`free` calls — and the latency jitter they introduce — from the hot path. This alone accounted for a >50% latency improvement over naive heap allocation.

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