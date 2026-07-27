[![CI](https://github.com/yashshelar466/High-Frequency-Limit-Order-Book/actions/workflows/ci.yml/badge.svg)](https://github.com/yashshelar466/High-Frequency-Limit-Order-Book/actions/workflows/ci.yml)

# High-Frequency Limit Order Book

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A single-threaded limit order book matching engine built in C++17, designed for low-latency execution and deterministic backtesting. Implements O(1) price-time priority matching, custom memory pool allocation, and a CSV-driven market data replayer for simulating tick-level trading strategies.

## Table of Contents
- [Overview](#overview)
- [Performance](#performance)
- [Design & Architecture](#design--architecture)
- [Key Optimizations](#key-optimizations)
- [Market Data Feed & Replayer](#market-data-feed--replayer)
- [Project Structure](#project-structure)
- [Quick Start](#quick-start)

## Overview

This project implements the core matching logic used in real exchange systems: orders are matched by **price priority**, then **time priority** (FIFO within a price level). The engine is single-threaded by design — this removes lock contention and makes execution fully deterministic, which matters for reproducible backtesting and for reasoning precisely about worst-case latency.

## Performance

Benchmarked on 100,000 randomized order operations (inserts, executions, cancellations), compiled with `-O3`. Timed using `QueryPerformanceCounter` for reliable nanosecond-resolution measurement on Windows.

| Metric               | Result                  |
| -------------------- | ------------------------ |
| Throughput           | 4.30M ops/sec             |
| Total Execution Time | ~23.28 ms (100k orders)   |

Latency is reported separately for resting inserts (no match) and crossing inserts (matched against the book), since a multi-level matching sweep is a fundamentally different cost than an O(1) resting insert — a single blended average hides that distinction.

**Resting inserts (no match)** — n=55,804

| Percentile | Latency    |
| ---------- | ---------- |
| avg        | 185.5 ns   |
| p50        | 200 ns     |
| p90        | 200 ns     |
| p99        | 400 ns     |
| p99.9      | 1,000 ns   |
| max        | 213,500 ns |

**Crossing inserts (matched against book)** — n=44,196

| Percentile | Latency    |
| ---------- | ---------- |
| avg        | 166.1 ns   |
| p50        | 100 ns     |
| p90        | 300 ns     |
| p99        | 500 ns     |
| p99.9      | 900 ns     |
| max        | 102,700 ns |

> The max values above are 100-200x larger than p99.9, which points to OS scheduler jitter (context switches, page faults) rather than the matching engine itself — the p99.9 column is the more representative worst case for the algorithm's actual behavior.

**Test Environment:** Windows 11, Intel Core i7 / AMD Ryzen 7, GCC 13.2 (MinGW-w64)
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
        │ (price → FIFO    │         │ (price → FIFO    │
        │  linked list)    │         │  linked list)    │
        └────────────────┘         └────────────────┘
                 │                           │
                 └─────────────┬─────────────┘
                               ▼
                    ┌─────────────────────┐
                    │   MemoryPool<Order>   │
                    │  placement-new alloc  │
                    └─────────────────────┘
```

Each price level is an intrusive doubly-linked list, so cancellation is O(1) (unlink in place, no shifting) and new orders at an existing price level append in O(1). Price levels themselves are indexed for O(1) best-bid/best-ask lookup rather than requiring a tree traversal.

## Key Optimizations

**Zero-allocation execution.** A custom contiguous `MemoryPool<Order>` uses placement `new` to pre-allocate order objects, eliminating `malloc`/`free` calls — and the latency jitter they introduce — from the hot path. This alone accounted for a >50% latency improvement over naive heap allocation.

**O(1) price-time matching.** Intrusive doubly-linked lists per price level, combined with direct array indexing into price levels, keep both order placement and cancellation constant-time regardless of book depth.

## Testing

Two layers of tests guard the matching engine:

- **Unit tests** (`tests/test_matching.cpp`) cover specific behaviors: partial fills, full level sweeps, FIFO preservation after a partial fill, cancellation, invalid inputs, duplicate-order-ID rejection, and top-of-book reset after a level is emptied.
- **Randomized differential test** (`tests/test_differential.cpp`) fires 300k random ADD / CANCEL / duplicate-ID / crossing operations at both the production engine and a deliberately simple `std::map`-based reference model, asserting they agree on top-of-book on every operation and on full per-level FIFO/volume state periodically. Any divergence points straight at a bug in the fast path — this is what catches whole classes of matching-engine bugs (stale best-price tracking, duplicate-ID handling) that example-based tests tend to miss.

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
- Git

### Build & Run

```bash
# Clone the repository
git clone https://github.com/yashshelar466/High-Frequency-Limit-Order-Book.git
cd High-Frequency-Limit-Order-Book

# Run unit tests
g++ -std=c++17 -Iinclude src/OrderBook.cpp tests/test_matching.cpp -o run_tests
./run_tests

# Run randomized differential test (fast engine vs. simple reference model)
g++ -O2 -std=c++17 -Iinclude src/OrderBook.cpp tests/test_differential.cpp -o run_diff
./run_diff

# Run latency & throughput benchmark
g++ -O3 -std=c++17 -Iinclude src/OrderBook.cpp benchmarks/benchmark_latency.cpp -o run_benchmark
./run_benchmark

# Run market data feed replayer
g++ -std=c++17 -Iinclude src/OrderBook.cpp src/replay_main.cpp -o run_feed
./run_feed
```

## License

Distributed under the MIT License. See `LICENSE` for more information.