#pragma once

#include "pitlane/util/hint.hpp"
#include "pitlane/util/numeric.hpp"

#include <cstdint>

namespace pitlane {

// Wire price as mantissa + decimal exponent. Convert once at the boundary into
// an engine tick-aligned integer. Hot paths keep using the engine integer.

struct WirePx {
    int64_t m{0};
    int8_t s{0};
};

PITLANE_INLINE bool scale_pow10(int64_t& v, int exp) noexcept {
    if (exp == 0) return true;
    if (exp > 0) {
        for (int i = 0; i < exp; ++i) {
            if (unlikely(v > INT64_MAX / 10 || v < INT64_MIN / 10)) return false;
            v *= 10;
        }
        return true;
    }
    for (int i = 0; i > exp; --i) v /= 10;
    return true;
}

PITLANE_INLINE bool to_engine(WirePx w, int64_t eng_sc, int64_t& out) noexcept {
    int64_t v = w.m;
    if (!scale_pow10(v, static_cast<int>(w.s))) return false;
    if (eng_sc <= 1) {
        out = v;
        return true;
    }
    if (unlikely(eng_sc <= 0)) return false;
    out = (v / eng_sc) * eng_sc;
    return true;
}

PITLANE_INLINE bool from_engine(int64_t px, int8_t wire_s, WirePx& out) noexcept {
    WirePx w{};
    w.s = wire_s;
    w.m = px;
    if (!scale_pow10(w.m, -static_cast<int>(wire_s))) return false;
    out = w;
    return true;
}

namespace detail {

inline bool u64_to_i64(uint64_t u, bool neg, int64_t& out) noexcept {
    if (!neg) {
        if (u > static_cast<uint64_t>(INT64_MAX)) return false;
        out = static_cast<int64_t>(u);
        return true;
    }
    if (u > static_cast<uint64_t>(INT64_MAX) + 1ull) return false;
    out = u == static_cast<uint64_t>(INT64_MAX) + 1ull ? INT64_MIN : -static_cast<int64_t>(u);
    return true;
}

}  // namespace detail

// (a/sa)*(b/sb) written at scale so. Uses Wide mul/div; fails on overflow.
PITLANE_INLINE bool fixed_mul(int64_t a, int64_t sa, int64_t b, int64_t sb, int64_t so, int64_t& out) noexcept {
    if (unlikely(sa <= 0 || sb <= 0 || so <= 0)) return false;
    const bool neg = (a < 0) != (b < 0);
    const uint64_t ua = static_cast<uint64_t>(a < 0 ? -a : a);
    const uint64_t ub = static_cast<uint64_t>(b < 0 ? -b : b);
    const uint64_t usa = static_cast<uint64_t>(sa);
    const uint64_t usb = static_cast<uint64_t>(sb);
    const uint64_t uso = static_cast<uint64_t>(so);
    Wide w = mul_wide(ua, ub);
    uint64_t q = 0;
    if (!div_wide(w, usa, q)) return false;
    w = mul_wide(q, uso);
    if (!div_wide(w, usb, q)) return false;
    return detail::u64_to_i64(q, neg, out);
}

}  // namespace pitlane
