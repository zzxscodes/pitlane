#pragma once

#include "pitlane/util/hint.hpp"
#include "pitlane/util/numeric.hpp"

#include <climits>
#include <cstdint>

namespace pitlane {

// Integer EMA with Q16 alpha = 2/(period+1) * 65536, rounded. Fail closed on overflow.

struct IntEma {
    int64_t v{0};
    uint32_t a_q16{0};
    uint8_t primed{0};
};

PITLANE_INLINE bool ema_open(IntEma& e, unsigned period, int64_t seed) noexcept {
    if (unlikely(period == 0)) return false;
    const uint64_t num = 2ull * 65536ull;
    const uint64_t den = static_cast<uint64_t>(period) + 1ull;
    const uint64_t a = (num + den / 2ull) / den;
    if (a == 0 || a > 65536ull) return false;
    e.a_q16 = static_cast<uint32_t>(a);
    e.v = seed;
    e.primed = 1;
    return true;
}

PITLANE_INLINE bool ema_push(IntEma& e, int64_t x) noexcept {
    if (unlikely(!e.primed)) return false;
    // v' = v + a*(x-v)/65536
    const int64_t d = x - e.v;
    const Wide prod = mul_wide(static_cast<uint64_t>(d < 0 ? -d : d), e.a_q16);
    uint64_t q = 0;
    if (!div_wide(prod, 65536ull, q)) return false;
    if (q > static_cast<uint64_t>(INT64_MAX)) return false;
    const int64_t step = static_cast<int64_t>(q);
    if (d < 0) {
        if (unlikely(e.v < INT64_MIN + step)) return false;
        e.v -= step;
    } else {
        if (unlikely(e.v > INT64_MAX - step)) return false;
        e.v += step;
    }
    return true;
}

// EMA of mean and of squared deviation proxy. std_out is abs(x - mean) smoothed.
struct IntEmaDev {
    IntEma mean{};
    IntEma absdev{};
};

PITLANE_INLINE bool ema_dev_open(IntEmaDev& d, unsigned period, int64_t seed) noexcept {
    return ema_open(d.mean, period, seed) && ema_open(d.absdev, period, 0);
}

PITLANE_INLINE bool ema_dev_push(IntEmaDev& d, int64_t x, int64_t& std_out) noexcept {
    if (!ema_push(d.mean, x)) return false;
    const int64_t abs_d = x >= d.mean.v ? x - d.mean.v : d.mean.v - x;
    if (!ema_push(d.absdev, abs_d)) return false;
    std_out = d.absdev.v;
    return true;
}

}  // namespace pitlane
