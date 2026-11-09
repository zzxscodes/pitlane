#pragma once

#include <cstdint>

namespace pitlane {

// One price band: [lo, hi) in the same integer unit as the price, step > 0.
struct PxBand {
    int64_t lo;
    int64_t hi;
    int64_t step;
};

inline const PxBand* band_for(const PxBand* bands, int n, int64_t px) noexcept {
    if (!bands || n <= 0) return nullptr;
    for (int i = 0; i < n; ++i) {
        if (px < bands[i].hi) return &bands[i];
    }
    return &bands[n - 1];
}

inline int64_t step_floor(const PxBand* bands, int n, int64_t px) noexcept {
    const PxBand* b = band_for(bands, n, px);
    if (!b || b->step <= 0) return px;
    if (px < b->lo) return px;
    const int64_t off = px - b->lo;
    return b->lo + (off / b->step) * b->step;
}

inline int64_t step_ceil(const PxBand* bands, int n, int64_t px) noexcept {
    const int64_t down = step_floor(bands, n, px);
    if (down == px) return px;
    const PxBand* b = band_for(bands, n, px);
    if (!b || b->step <= 0) return px;
    return down + b->step;
}

// Lookup that keeps a price sitting exactly on the next band's start in the lower band.
inline int band_at_hold(const PxBand* bands, int n, int64_t px) noexcept {
    if (!bands || n <= 0) return -1;
    int i = 0;
    while (i + 1 < n && px > bands[i + 1].lo) ++i;
    return i;
}

// Clamp only when up is strictly above down. Otherwise the price is unchanged.
inline int64_t clamp_px(int64_t px, int64_t down, int64_t up) noexcept {
    if (up <= down) return px;
    if (px < down) return down;
    if (px > up) return up;
    return px;
}

// Walk `steps` ticks. A price that lands on the next band's start uses the new step afterwards.
// A downward walk that starts exactly on a band start takes the first tick from the lower band.
inline int64_t walk_up(const PxBand* bands, int n, int64_t px, int steps, int64_t down, int64_t up) noexcept {
    if (steps <= 0 || !bands || n <= 0) return clamp_px(px, down, up);
    int i = band_at_hold(bands, n, px);
    if (i < 0) return clamp_px(px, down, up);
    for (int k = 0; k < steps; ++k) {
        if (bands[i].step <= 0) break;
        if (px > INT64_MAX - bands[i].step) {
            px = INT64_MAX;
            break;
        }
        px += bands[i].step;
        if (i + 1 < n && px >= bands[i + 1].lo) ++i;
    }
    return clamp_px(px, down, up);
}

inline int64_t walk_down(const PxBand* bands, int n, int64_t px, int steps, int64_t down, int64_t up) noexcept {
    if (steps <= 0 || !bands || n <= 0) return clamp_px(px, down, up);
    int i = band_at_hold(bands, n, px);
    if (i < 0) return clamp_px(px, down, up);
    if (i > 0 && px == bands[i].lo) --i;
    for (int k = 0; k < steps; ++k) {
        if (bands[i].step <= 0) break;
        if (px < bands[i].step) px = 0;
        else px -= bands[i].step;
        if (px < 0) px = 0;
        if (i > 0 && px <= bands[i].lo) --i;
    }
    return clamp_px(px, down, up);
}

}  // namespace pitlane
