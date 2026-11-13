#include "perf_suite.hpp"

#include <cstdio>

// Paper-center timings for the simnow example, plus the measurement helpers those timings use.
// No broker socket. The live gateway is not on this path.

int main() {
    std::printf("pitlane_simnow_bench %s\n", pitlane::kVersion);
    const int fails = pitlane_bench::run();
    if (fails != 0) {
        std::fprintf(stderr, "%d bench check(s) failed\n", fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
