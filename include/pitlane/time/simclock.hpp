#pragma once

#include "pitlane/book/lags.hpp"

#include <cstdint>

namespace pitlane {

// Sampled go/back delays when points are loaded, otherwise the nine session bands.
struct SimClock {
    LagTable table{};
    LagPoint pts[64]{};
    int npts{0};
    int go_cursor{0};
    int back_cursor{0};

    int64_t go(int64_t now) noexcept;
    int64_t back(int64_t exch) noexcept;
};

}  // namespace pitlane
