# High-Frequency Limit Order Book

A high-performance, single-threaded Limit Order Book matching engine built in **C++17**, engineered for low-latency execution and deterministic backtesting. Features an $O(1)$ price-time priority matching algorithm, custom memory pool allocation, and a real-time market data CSV tick replayer.

---

## Performance & Benchmarks

Benchmarked on **100,000 randomized order operations** (inserts, executions, cancellations) using `-O3` compiler optimizations:

| Metric | Result |
| :--- | :--- |
| **Average Latency** | **225.74 ns** / order |
| **Throughput** | **3.25 Million** ops/sec |
| **Execution Time** | **~30.76 ms** (for 100k orders) |

### Key Optimizations
* **Zero-Allocation Execution:** Custom contiguous `MemoryPool<Order>` using placement `new` eliminates dynamic OS heap overhead (`malloc`/`free`) and latency jitter, improving performance by **>50%**.
* **$O(1)$ Price-Time Matching:** Intrusive doubly-linked lists per price level combined with direct array indexing ensure constant-time order placement and cancellation.

---

## Market Data Feed & Replayer

Includes an event-driven `MarketDataFeed` parser capable of streaming high-frequency tick data into the engine:
* Processes `ADD`, `CANCEL`, and `EXECUTE` order events in real time.
* Supports timestamp-based playback simulation for backtesting trading strategies.

---

## Quick Start

### Prerequisites
* C++17 compatible compiler (`g++` / GCC or Clang)
* Git

### Build & Run Instructions

```bash
# 1. Clone the repository
git clone [https://github.com/YOUR_GITHUB_USERNAME/Limit-Order-Book.git](https://github.com/YOUR_GITHUB_USERNAME/Limit-Order-Book.git)
cd Limit-Order-Book

# 2. Run Unit Tests
g++ -std=c++17 -Iinclude src/OrderBook.cpp tests/test_matching.cpp -o run_tests
./run_tests

# 3. Run Latency & Throughput Benchmark
g++ -O3 -std=c++17 -Iinclude src/OrderBook.cpp benchmarks/benchmark_latency.cpp -o run_benchmark
./run_benchmark

# 4. Run Market Data Feed Replayer
g++ -std=c++17 -Iinclude src/OrderBook.cpp src/replay_main.cpp -o run_feed
./run_feed