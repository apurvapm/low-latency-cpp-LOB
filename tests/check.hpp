#pragma once

#include <cstdio>

// CHECK(cond) is assert() that isn't compiled out by NDEBUG, so it still
// verifies something in the default Release build (the old tests
// used bare assert() and CMAKE_BUILD_TYPE=Release defines NDEBUG, so
// `ctest` passed without checking anything).
namespace lob::test {

    inline int& failureCount() noexcept {
        static int count = 0;
        return count;
    }

    inline void recordFailure(const char* expr, const char* file, int line) noexcept {
        std::fprintf(stderr, "CHECK failed: %s at %s:%d\n", expr, file, line);
        ++failureCount();
    }

} // namespace lob::test

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ::lob::test::recordFailure(#cond, __FILE__, __LINE__);           \
        }                                                                    \
    } while (0)
