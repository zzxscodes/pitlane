#pragma once

#include <cstdint>

namespace pitlane {

// Sticky first-breach and pool watermarks for monitor paths (notes monitoring §1, §4).
// No sockets. Caller supplies samples.

inline bool pool_low(uint64_t used, uint64_t cap, uint32_t warn_ppm) noexcept {
    if (cap == 0 || warn_ppm > 1000000)
        return true;
    return used * 1000000ull >= cap * static_cast<uint64_t>(warn_ppm);
}

struct BreachFlag {
    uint8_t latched{0};
    int64_t first_at{0};
};

// Returns true on the first crossing of the high threshold. Stays latched until clear.
inline bool first_breach_high(BreachFlag& f, int64_t sample, int64_t high, int64_t now) noexcept {
    if (f.latched)
        return false;
    if (sample < high)
        return false;
    f.latched = 1;
    f.first_at = now;
    return true;
}

inline bool first_breach_low(BreachFlag& f, int64_t sample, int64_t low, int64_t now) noexcept {
    if (f.latched)
        return false;
    if (sample > low)
        return false;
    f.latched = 1;
    f.first_at = now;
    return true;
}

inline void breach_clear(BreachFlag& f) noexcept {
    f.latched = 0;
    f.first_at = 0;
}

} // namespace pitlane
