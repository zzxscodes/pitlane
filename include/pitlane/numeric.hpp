#pragma once

#include <cstdint>

namespace pitlane {

inline int sign_of(int64_t v) noexcept { return (v > 0) - (v < 0); }
inline int sign_of(double v) noexcept { return (v > 0) - (v < 0); }

inline int64_t round_down_to(int64_t v, int64_t step) noexcept {
    if (step <= 0) return v;
    if (v >= 0) return (v / step) * step;
    const int64_t q = v / step;
    return q * step == v ? v : (q - 1) * step;
}

inline int64_t round_up_to(int64_t v, int64_t step) noexcept {
    if (step <= 0) return v;
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
