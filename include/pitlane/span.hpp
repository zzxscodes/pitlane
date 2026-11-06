#pragma once

#include <cstdint>

namespace pitlane {

// Timestamps kept beside a message. Unset nodes stay at -1, so a zero tick is a real sample.
template <int N>
struct SpanMark {
    static_assert(N >= 2 && N <= 16, "span nodes");

    int64_t ts[N]{};
    uint64_t seq{0};

    SpanMark() noexcept {
        for (int i = 0; i < N; ++i) ts[i] = -1;
    }

    bool mark(int node, int64_t ticks) noexcept {
        if (node < 0 || node >= N || ticks < 0) return false;
        ts[node] = ticks;
        return true;
    }

    bool between(int earlier, int later, int64_t& out) const noexcept {
        if (earlier < 0 || later < 0 || earlier >= N || later >= N) return false;
        if (ts[earlier] < 0 || ts[later] < 0) return false;
        out = ts[later] - ts[earlier];
        return true;
    }
};

}  // namespace pitlane
