#pragma once

#include "pitlane/util/simd.hpp"

#include <cstdint>

namespace pitlane {

// How much visible quantity can meet a limit. A buy counts prices at or below the limit.
// A sell counts prices at or above the limit. The result stops at need.
// Buy and sell are separate loops so each path stays branch-light and can use packed compares.
// The arrays are not changed, and the call does not match or send.

PITLANE_HOT inline int64_t cover_qty(const int64_t* __restrict px, const int64_t* __restrict qty, int n, bool buy,
                                     int64_t limit, int64_t need) noexcept {
    return buy ? cover_qty_buy(px, qty, n, limit, need) : cover_qty_sell(px, qty, n, limit, need);
}

}  // namespace pitlane
