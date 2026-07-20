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
