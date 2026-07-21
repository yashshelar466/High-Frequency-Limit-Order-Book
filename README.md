# Limit-Order-Book
A high-performance, single-threaded Limit Order Book simulator in C++20 optimized for low-latency execution and deterministic backtesting. Features cache-aligned memory pools, O(1) order operations, and Nasdaq ITCH binary data parsing.

##  Performance & Benchmarks

Benchmarked processing 100,000 randomized orders (inserts, matches, cancellations):

| Metric | Result |
| :--- | :--- |
| **Average Latency** | **487.91 ns** / order |
| **Throughput** | **1.57 Million** ops/sec |
| **Execution Time** | ~63.7 ms |

*Compiled with GCC `-O3` optimizations.*

##  Market Data Feed & Replayer

Includes an event-driven `MarketDataFeed` parser capable of streaming high-frequency tick data (CSV/binary feeds) into the engine:
* Processes `ADD`, `CANCEL`, and `EXECUTE` order events in real time.
* Supports timestamp-based playback simulation for backtesting trading strategies.

##  Performance & Benchmarks

* **Average Latency:** `~225.74 nanoseconds` per order operation
* **Throughput:** `~3.25 Million operations/sec`
* **Memory Management:** Custom zero-allocation `MemoryPool<Order>` eliminates OS heap overhead and latency jitter.