#pragma once

#include "pitlane/codec.hpp"

#include <cstdint>

namespace pitlane {

struct LotPos {
    int64_t total{0};
    int64_t today{0};
};

struct LotPend {
    int64_t close_today{0};
    int64_t close_yday{0};
};

struct LotSlice {
    Off off{Off::Open};
    int64_t qty{0};
};

enum class SplitMode : uint8_t { TodayThenYdayThenOpen = 1, CloseOnly = 2, YdayThenStop = 3 };

// Returns the number of slices written, or -1 for an unsupported mode.
// A slice smaller than min_lot is dropped. max_lot <= 0 means no cap.
int split_lots(int64_t want, LotPos pos, LotPend pend, int64_t max_lot, int64_t min_lot, SplitMode mode, LotSlice* out,
               int cap) noexcept;

}  // namespace pitlane
