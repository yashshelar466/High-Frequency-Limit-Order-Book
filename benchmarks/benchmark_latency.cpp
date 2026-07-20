#include "../include/OrderBook.hpp"
#include <chrono>
#include <iostream>
#include <vector>
#include <numeric>
#include <random>

int main() {
    constexpr int NUM_ORDERS = 100000;
    OrderBook book;

    std::cout << "--- Running Latency Benchmark (" << NUM_ORDERS << " orders) ---" << std::endl;

    // Fast PRNG for generating test order data
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> price_dist(90, 110);
    std::uniform_int_distribution<uint32_t> qty_dist(10, 500);
    std::uniform_int_distribution<int> side_dist(0, 1);

    std::vector<int64_t> latencies_ns;
    latencies_ns.reserve(NUM_ORDERS);

    auto start_total = std::chrono::high_resolution_clock::now();

    for (uint64_t id = 1; id <= NUM_ORDERS; ++id) {
        uint32_t price = price_dist(rng);
        uint32_t qty = qty_dist(rng);
        bool is_buy = side_dist(rng) == 1;

        auto t1 = std::chrono::high_resolution_clock::now();
        
        book.insert_order(id, price, qty, is_buy);
        
        auto t2 = std::chrono::high_resolution_clock::now();

        int64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        latencies_ns.push_back(elapsed_ns);
    }

    auto end_total = std::chrono::high_resolution_clock::now();
    double total_time_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();

    // Calculate metrics
    int64_t sum_latencies = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0LL);
    double avg_latency_ns = static_cast<double>(sum_latencies) / NUM_ORDERS;
    double throughput_ops_sec = (NUM_ORDERS / total_time_ms) * 1000.0;

    std::cout << "\n=== BENCHMARK RESULTS ===" << std::endl;
    std::cout << "Total Orders Processed: " << NUM_ORDERS << std::endl;
    std::cout << "Total Wall Clock Time:  " << total_time_ms << " ms" << std::endl;
    std::cout << "Average Latency:        " << avg_latency_ns << " ns / order" << std::endl;
    std::cout << "Throughput:             " << throughput_ops_sec / 1e6 << " Million ops/sec" << std::endl;
    std::cout << "=========================" << std::endl;

    return 0;
}