#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Sticky on/off latches for parent/algo pacing. Integer only; no float.

struct PaceLatch {
    uint8_t cross_on{0};
    uint8_t trim_on{0};
};

PITLANE_INLINE void latch_cross(PaceLatch& p, int64_t parent, int64_t aim, int64_t done, int cancel_ppm) noexcept {
    if (unlikely(parent <= 0)) return;
    const int64_t gap = aim - done;
    if (!p.cross_on && gap * 20 > parent) p.cross_on = 1;
    if (p.cross_on && gap * 100 < parent * 3 && cancel_ppm < 550000) p.cross_on = 0;
}

PITLANE_INLINE void latch_trim(PaceLatch& p, int64_t parent, int64_t working, int64_t lag) noexcept {
    if (unlikely(parent <= 0)) return;
    const int64_t behind = lag > 0 ? lag : 0;
    if (!p.trim_on && working * 10 >= parent + behind * 10) p.trim_on = 1;
    if (p.trim_on && working * 20 < parent + behind * 20) p.trim_on = 0;
}

// Larger of floor_us and span_us * ratio_ppm / 1_000_000.
PITLANE_INLINE int64_t window_back(int64_t span_us, int64_t floor_us, int64_t ratio_ppm) noexcept {
    if (unlikely(span_us < 0 || floor_us < 0 || ratio_ppm < 0)) return floor_us > 0 ? floor_us : 0;
    const int64_t by_ratio = span_us * ratio_ppm / 1000000;
    return by_ratio > floor_us ? by_ratio : floor_us;
}

// Band hysteresis for in-slice wait (notes §48). enter_ppm turns on; exit_ppm turns off.
struct HysteresisGate {
    uint8_t on{0};
};

PITLANE_INLINE void hyst_update(HysteresisGate& g, int64_t sample, int64_t enter, int64_t exit_at) noexcept {
    if (enter < exit_at) return;  // require enter >= exit for high-side band
    if (!g.on && sample >= enter) g.on = 1;
    else if (g.on && sample <= exit_at) g.on = 0;
}

// Flatten residual working qty at end of day when at or below floor lots.
PITLANE_INLINE int64_t end_flat_pull(int64_t working, int64_t floor_lots) noexcept {
    if (working <= 0) return 0;
    if (floor_lots < 0) floor_lots = 0;
    return working <= floor_lots ? working : 0;
}

}  // namespace pitlane
