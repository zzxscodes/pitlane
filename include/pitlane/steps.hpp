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

}  // namespace pitlane
