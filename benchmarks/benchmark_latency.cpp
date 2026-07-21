#include "../include/OrderBook.hpp"
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <random>
#include <cmath>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// std::chrono::steady_clock on some MinGW/Windows builds does not reliably
// resolve down to the sub-microsecond scale a single order insert takes
// (~100-300ns) -- most measurements round down to 0. QueryPerformanceCounter
// is the actual high-resolution timer on Windows and gives real nanosecond
// granularity; on other platforms steady_clock is already reliable.
struct HighResTimer {
#ifdef _WIN32
    static double ticks_per_sec() {
        static LARGE_INTEGER freq;
        static bool initialized = (QueryPerformanceFrequency(&freq), true);
        (void)initialized;
        return static_cast<double>(freq.QuadPart);
    }
    static int64_t now_ns() {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<int64_t>(
            (static_cast<double>(counter.QuadPart) / ticks_per_sec()) * 1e9);
    }
#else
    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
#endif
};

// Returns the value at the given percentile (0-100) of an already-sorted vector.
int64_t percentile(const std::vector<int64_t>& sorted_latencies, double pct) {
    if (sorted_latencies.empty()) return 0;
    size_t idx = static_cast<size_t>(std::ceil(pct / 100.0 * sorted_latencies.size())) - 1;
    idx = std::min(idx, sorted_latencies.size() - 1);
    return sorted_latencies[idx];
}

void print_stats(const std::string& label, std::vector<int64_t> latencies_ns) {
    if (latencies_ns.empty()) {
        std::cout << label << ": no samples" << std::endl;
        return;
    }
    std::sort(latencies_ns.begin(), latencies_ns.end());
    int64_t sum = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0LL);
    double avg = static_cast<double>(sum) / latencies_ns.size();

    std::cout << label << " (n=" << latencies_ns.size() << ")\n"
              << "  avg:   " << avg << " ns\n"
              << "  p50:   " << percentile(latencies_ns, 50) << " ns\n"
              << "  p90:   " << percentile(latencies_ns, 90) << " ns\n"
              << "  p99:   " << percentile(latencies_ns, 99) << " ns\n"
              << "  p99.9: " << percentile(latencies_ns, 99.9) << " ns\n"
              << "  max:   " << latencies_ns.back() << " ns\n";
}

}  // namespace

int main() {
    constexpr int NUM_ORDERS = 100000;
    OrderBook book;

    std::cout << "--- Running Latency Benchmark (" << NUM_ORDERS << " orders) ---" << std::endl;

    // Fast PRNG for generating test order data
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> price_dist(90, 110);
    std::uniform_int_distribution<uint32_t> qty_dist(10, 500);
    std::uniform_int_distribution<int> side_dist(0, 1);

    // Track resting inserts and crossing/matching inserts separately —
    // averaging them together hides that a matching sweep across several
    // price levels is a fundamentally different cost than an O(1) resting
    // insert, which matters more than the average for HFT-style latency work.
    std::vector<int64_t> resting_latencies_ns;
    std::vector<int64_t> crossing_latencies_ns;
    resting_latencies_ns.reserve(NUM_ORDERS);
    crossing_latencies_ns.reserve(NUM_ORDERS);

    int64_t start_total_ns = HighResTimer::now_ns();

    for (uint64_t id = 1; id <= NUM_ORDERS; ++id) {
        uint32_t price = price_dist(rng);
        uint32_t qty = qty_dist(rng);
        bool is_buy = side_dist(rng) == 1;

        // Determine ahead of time whether this order will cross the book,
        // so we can bucket its latency correctly after the call.
        bool will_cross = is_buy
            ? (price >= book.get_best_ask())
            : (price <= book.get_best_bid());

        int64_t t1 = HighResTimer::now_ns();

        book.insert_order(id, price, qty, is_buy);

        int64_t t2 = HighResTimer::now_ns();

        int64_t elapsed_ns = t2 - t1;
        if (will_cross) {
            crossing_latencies_ns.push_back(elapsed_ns);
        } else {
            resting_latencies_ns.push_back(elapsed_ns);
        }
    }

    int64_t end_total_ns = HighResTimer::now_ns();
    double total_time_ms = static_cast<double>(end_total_ns - start_total_ns) / 1e6;
    double throughput_ops_sec = (NUM_ORDERS / total_time_ms) * 1000.0;

    std::cout << "\n=== BENCHMARK RESULTS ===\n";
    std::cout << "Total Orders Processed: " << NUM_ORDERS << "\n";
    std::cout << "Total Wall Clock Time:  " << total_time_ms << " ms\n";
    std::cout << "Throughput:             " << throughput_ops_sec / 1e6 << " Million ops/sec\n\n";
    print_stats("Resting inserts (no match)", resting_latencies_ns);
    std::cout << "\n";
    print_stats("Crossing inserts (matched against book)", crossing_latencies_ns);
    std::cout << "=========================" << std::endl;

    return 0;
}