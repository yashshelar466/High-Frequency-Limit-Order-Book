#include "../include/MemoryPool.hpp"
#include "check.hpp"
#include <string>
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

    // pool destructor runs here — must not re-destroy anything.
    std::cout << "[PASS] MemoryPool non-trivial type (no double free)" << std::endl;
    return 0;
}