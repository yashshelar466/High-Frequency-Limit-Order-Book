#pragma once
#include <cstdio>
#include <cstdlib>

// A correctness check that is NEVER compiled out (unlike assert under NDEBUG).
// Implemented via a helper function so the macro has NO line-continuation
// backslashes — it can't be silently broken by a truncated copy-paste.
inline void check_fail(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "CHECK FAILED: %s\n  at %s:%d\n", expr, file, line);
    std::exit(1);
}

#define CHECK(cond) ((cond) ? (void)0 : check_fail(#cond, __FILE__, __LINE__))