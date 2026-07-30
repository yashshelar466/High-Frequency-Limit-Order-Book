#include "../include/MemoryPool.hpp"
#include "check.hpp"
#include <string>
#include <vector>
#include <iostream>

int main() {
    // std::string is non-trivially-destructible: the old vector<T>-backed pool
    // destroyed each one twice (deallocate + backing-vector teardown), a double
    // free. This must be clean now — and clean under ASan in CI.
    MemoryPool<std::string> pool;

    std::string* a = pool.allocate("a long std::string that owns a heap buffer aaaaaaaaaaaaaaaa");
    std::string* b = pool.allocate("another long std::string owning its own heap buffer bbbbbbbb");
    CHECK(*a == "a long std::string that owns a heap buffer aaaaaaaaaaaaaaaa");
    CHECK(*b == "another long std::string owning its own heap buffer bbbbbbbb");

    pool.deallocate(a);
    pool.deallocate(b);

    std::string* c = pool.allocate("reused slot");   // free-list still works
    CHECK(*c == "reused slot");
    pool.deallocate(c);

    CHECK(!pool.has_overflowed());          // stayed within capacity throughout
    CHECK(pool.overflow_total() == 0);

    // --- Overflow path -----------------------------------------------------
    // deallocate() skips the heap-pointer lookup entirely while the pool has
    // never overflowed. That fast path is only safe if the flag is exact: a
    // heap pointer mistaken for a pooled one would be placement-destroyed and
    // then pushed onto the free list as if it were pool storage — heap
    // corruption, and a later allocate() handing out a pointer into freed
    // memory. So drive a tiny pool past capacity and exercise both routes,
    // interleaved. ASan/UBSan in CI is what makes this test bite.
    {
        constexpr size_t CAP = 4;
        MemoryPool<std::string, CAP> small;

        std::vector<std::string*> pooled;
        for (size_t i = 0; i < CAP; ++i)
            pooled.push_back(small.allocate("pooled slot, long enough to own a heap buffer"));
        CHECK(small.available() == 0);
        CHECK(!small.has_overflowed());

        // Past capacity: these come from the real heap.
        std::string* spill1 = small.allocate("spill one, also owning its own heap buffer");
        std::string* spill2 = small.allocate("spill two, also owning its own heap buffer");
        CHECK(small.has_overflowed());
        CHECK(small.overflow_total() == 2);
        CHECK(small.heap_overflow_count() == 2);
        CHECK(*spill1 == "spill one, also owning its own heap buffer");
        CHECK(*spill2 == "spill two, also owning its own heap buffer");

        // Interleave the two kinds of release: each pointer must go down the
        // route it actually came from, in any order.
        small.deallocate(pooled[0]);
        small.deallocate(spill1);
        small.deallocate(pooled[1]);
        CHECK(small.heap_overflow_count() == 1);   // spill2 still live
        CHECK(small.available() == 2);             // two slots back on the free list

        // A returned pool slot must be genuinely reusable, not a heap pointer
        // that was wrongly filed into the free list.
        std::string* reused = small.allocate("reused after mixed release");
        CHECK(*reused == "reused after mixed release");
        CHECK(small.available() == 1);

        small.deallocate(reused);
        small.deallocate(spill2);
        small.deallocate(pooled[2]);
        small.deallocate(pooled[3]);
        CHECK(small.heap_overflow_count() == 0);   // every heap object released
        CHECK(small.available() == CAP);           // every slot back
        CHECK(small.overflow_total() == 2);        // total is cumulative, not live
    }

    // pool destructor runs here — must not re-destroy anything.
    std::cout << "[PASS] MemoryPool non-trivial type (no double free)" << std::endl;
    std::cout << "[PASS] MemoryPool overflow path (pooled/heap release routing)" << std::endl;
    return 0;
}