#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

PITLANE_INLINE int sign_of(int64_t v) noexcept { return (v > 0) - (v < 0); }
PITLANE_INLINE int sign_of(double v) noexcept { return (v > 0) - (v < 0); }

PITLANE_INLINE int64_t round_down_to(int64_t v, int64_t step) noexcept {
    if (unlikely(step <= 0)) return v;
    if (likely(v >= 0)) return (v / step) * step;
    const int64_t q = v / step;
    return q * step == v ? v : (q - 1) * step;
}

PITLANE_INLINE int64_t round_up_to(int64_t v, int64_t step) noexcept {
    if (unlikely(step <= 0)) return v;
    const int64_t down = round_down_to(v, step);
    return down == v ? v : down + step;
}

inline int64_t round_nearest_to(int64_t v, int64_t step) noexcept {
    if (step <= 0) return v;
    const int64_t down = round_down_to(v, step);
    const int64_t up = down == v ? v : down + step;
    const int64_t ld = v >= down ? v - down : down - v;
    const int64_t lu = up >= v ? up - v : v - up;
    if (ld < lu) return down;
    if (lu < ld) return up;
    return v >= 0 ? up : down;
}

inline int64_t round_up_nonzero(int64_t v, int64_t step) noexcept {
    if (v == 0) return step > 0 ? step : 0;
    return round_up_to(v, step);
}

inline int64_t max_abs(int64_t a, int64_t b) noexcept {
    const int64_t aa = a < 0 ? -a : a;
    const int64_t bb = b < 0 ? -b : b;
    return aa >= bb ? a : b;
}

inline int64_t min_abs(int64_t a, int64_t b) noexcept {
    const int64_t aa = a < 0 ? -a : a;
    const int64_t bb = b < 0 ? -b : b;
    return aa <= bb ? a : b;
}

// Unsigned 64x64 product. Used so ratio checks do not overflow a signed 64-bit multiply.
struct Wide {
    uint64_t hi{0};
    uint64_t lo{0};
};

PITLANE_HOT PITLANE_INLINE Wide mul_wide(uint64_t a, uint64_t b) noexcept {
    const uint64_t a0 = a & 0xffffffffull;
    const uint64_t a1 = a >> 32;
    const uint64_t b0 = b & 0xffffffffull;
    const uint64_t b1 = b >> 32;
    const uint64_t p0 = a0 * b0;
    const uint64_t p1 = a0 * b1;
    const uint64_t p2 = a1 * b0;
    const uint64_t p3 = a1 * b1;
    const uint64_t mid = (p0 >> 32) + (p1 & 0xffffffffull) + (p2 & 0xffffffffull);
    Wide w;
    w.lo = (p0 & 0xffffffffull) | (mid << 32);
    w.hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    return w;
}

PITLANE_INLINE int cmp_wide(Wide a, Wide b) noexcept {
    if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
    return 0;
}

// Compare a*b ? c*d for non-negative values. Returns -1, 0, or 1.
inline int cmp_prod(int64_t a, int64_t b, int64_t c, int64_t d) noexcept {
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (c < 0) c = 0;
    if (d < 0) d = 0;
    return cmp_wide(mul_wide(static_cast<uint64_t>(a), static_cast<uint64_t>(b)),
                    mul_wide(static_cast<uint64_t>(c), static_cast<uint64_t>(d)));
}

// Quotient of a 128-bit value and a 64-bit divisor. Fails when the quotient does not fit in 64 bits.
// den must be small enough that a remainder below den can be shifted by one bit.
PITLANE_HOT bool div_wide(Wide w, uint64_t den, uint64_t& q) noexcept {
    if (unlikely(den == 0 || den > (UINT64_MAX >> 1) || w.hi >= den)) return false;
    uint64_t rem = 0;
    for (int i = 63; i >= 0; --i) {
        rem = (rem << 1) | ((w.hi >> static_cast<unsigned>(i)) & 1ull);
        if (rem >= den) rem -= den;
    }
    q = 0;
    for (int i = 63; i >= 0; --i) {
        const uint64_t bit = (w.lo >> static_cast<unsigned>(i)) & 1ull;
        rem = (rem << 1) | bit;
        q <<= 1;
        if (rem >= den) {
            rem -= den;
            q |= 1ull;
        }
    }
    return true;
}

inline char digit_char(int v) noexcept { return (v >= 0 && v <= 9) ? static_cast<char>('0' + v) : '?'; }

inline int to_hex(int64_t v, char* out, int cap) noexcept {
    if (!out || cap < 2) return 0;
    static constexpr char kHex[] = "0123456789abcdef";
    char tmp[16];
    uint64_t u = static_cast<uint64_t>(v);
    int n = 0;
    do {
        tmp[n++] = kHex[u & 0xfu];
        u >>= 4;
    } while (u && n < 16);
    if (n + 1 > cap) return 0;
    for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

}  // namespace pitlane
