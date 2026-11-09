#pragma once

#include <cstdint>

namespace pitlane {

// How much visible quantity can meet a limit. A buy counts prices at or below the limit.
// A sell counts prices at or above the limit. The result stops at need.
// The arrays are not changed, and the call does not match or send.

inline int64_t cover_qty(const int64_t* px, const int64_t* qty, int n, bool buy, int64_t limit, int64_t need) noexcept {
    if (!px || !qty || n <= 0 || limit <= 0 || need <= 0) return 0;
    int64_t got = 0;
    for (int i = 0; i < n && got < need; ++i) {
        if (px[i] <= 0 || qty[i] <= 0) continue;
        if (buy) {
            if (px[i] > limit) continue;
        } else if (px[i] < limit) {
            continue;
        }
        const int64_t room = need - got;
        got += qty[i] < room ? qty[i] : room;
    }
    return got;
}

}  // namespace pitlane
