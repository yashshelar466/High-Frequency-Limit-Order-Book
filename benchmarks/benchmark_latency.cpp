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

int64_t percentile(const std::vector<int64_t>& sorted, double pct) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(std::ceil(pct / 100.0 * sorted.size())) - 1;
    idx = std::min(idx, sorted.size() - 1);
    return sorted[idx];
}

void print_stats(const std::string& label, std::vector<int64_t> lat) {
    if (lat.empty()) { std::cout << label << ": no samples\n"; return; }
    std::sort(lat.begin(), lat.end());
    int64_t sum = std::accumulate(lat.begin(), lat.end(), 0LL);
    std::cout << label << " (n=" << lat.size() << ")\n"
              << "  avg:   " << static_cast<double>(sum) / lat.size() << " ns\n"
              << "  p50:   " << percentile(lat, 50) << " ns\n"
              << "  p90:   " << percentile(lat, 90) << " ns\n"
              << "  p99:   " << percentile(lat, 99) << " ns\n"
              << "  p99.9: " << percentile(lat, 99.9) << " ns\n"
              << "  max:   " << lat.back() << " ns\n";
}

// Median cost of an empty now_ns() bracket — the stopwatch's own overhead.
int64_t timer_overhead_ns() {
    std::vector<int64_t> s;
    s.reserve(200000);
    for (int i = 0; i < 200000; ++i) {
        int64_t a = HighResTimer::now_ns();
        int64_t b = HighResTimer::now_ns();
        s.push_back(b - a);
    }
    std::sort(s.begin(), s.end());
    return percentile(s, 50);
}

// Resting-insert p50/p99 into a one-sided book holding ~num_levels price
// levels, so the O(log L) cost is visible instead of hidden behind a tiny band.
void bench_depth(uint32_t num_levels) {
    constexpr int OPS = 50000;
    OrderBook book;
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> price(1000, 1000 + num_levels - 1);
    std::uniform_int_distribution<uint32_t> qty(10, 500);

    uint64_t id = 1;
    for (uint32_t p = 1000; p < 1000 + num_levels; ++p)
        book.insert_order(id++, p, 100, /*is_buy=*/true);   // pre-fill L levels

    std::vector<int64_t> lat; lat.reserve(OPS);
    for (int i = 0; i < OPS; ++i) {
        uint32_t p = price(rng);
        int64_t t1 = HighResTimer::now_ns();
        book.insert_order(id++, p, qty(rng), true);         // rests, never crosses
        int64_t t2 = HighResTimer::now_ns();
        lat.push_back(t2 - t1);
    }
    std::sort(lat.begin(), lat.end());
    std::cout << "  L=" << num_levels << " levels:  p50=" << percentile(lat, 50)
              << " ns   p99=" << percentile(lat, 99) << " ns\n";
}

// The intrusive-list cancel path — the headline design decision, previously
// unmeasured. Cancels hit random queue positions.
void bench_cancel() {
    constexpr int OPS = 80000;
    OrderBook book;
    std::mt19937 rng(7);
    std::uniform_int_distribution<uint32_t> price(1000, 1200);
    std::vector<uint64_t> ids; ids.reserve(OPS);
    uint64_t id = 1;
    for (int i = 0; i < OPS; ++i) { book.insert_order(id, price(rng), 100, true); ids.push_back(id++); }
    std::shuffle(ids.begin(), ids.end(), rng);

    std::vector<int64_t> lat; lat.reserve(OPS);
    for (uint64_t cid : ids) {
        int64_t t1 = HighResTimer::now_ns();
        book.cancel_order(cid);
        int64_t t2 = HighResTimer::now_ns();
        lat.push_back(t2 - t1);
    }
    print_stats("Cancels (intrusive-list unlink)", lat);
}

}  // namespace

int main() {
    int64_t overhead = timer_overhead_ns();
    std::cout << "--- Latency Benchmark ---\n";
    std::cout << "Timer overhead (empty bracket, p50): " << overhead
              << " ns  <-- subtract this from every figure for pure engine cost\n\n";

    std::cout << "Resting-insert latency vs. book depth (one-sided, no crossing):\n";
    bench_depth(21);
    bench_depth(1000);
    bench_depth(20000);
    std::cout << "\n";

    bench_cancel();

    std::cout << "=========================" << std::endl;
    return 0;
}