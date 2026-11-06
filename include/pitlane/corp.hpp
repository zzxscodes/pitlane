#pragma once

#include "pitlane/numeric.hpp"

#include <cstdint>

namespace pitlane {

// A corporate action already in force on ymd. Later actions are left out, so an older adjusted
// price does not change when a new action is appended. The jump is the raw price ratio.
// A split of 1 into 2 uses jump 1/2. Cash dividends change the factor only when income is set.

struct CorpAct {
    int ymd{0};
    int64_t jump_num{1};
    int64_t jump_den{1};
    int64_t prev_close{0};
    int64_t cash_div{0};
    uint8_t income{0};
};

struct PxMul {
    int64_t num{1};
    int64_t den{1};
};

inline int64_t gcd_pos(int64_t a, int64_t b) noexcept {
    if (a < 0) a = a == INT64_MIN ? INT64_MAX : -a;
    if (b < 0) b = b == INT64_MIN ? INT64_MAX : -b;
    while (b != 0) {
        const int64_t t = a % b;
        a = b;
        b = t;
    }
    return a == 0 ? 1 : a;
}

inline bool mul_frac(int64_t& num, int64_t& den, int64_t n2, int64_t d2) noexcept {
    if (den <= 0 || d2 <= 0 || num == 0 || n2 == 0) return false;
    const int neg = (num < 0) ^ (n2 < 0);
    int64_t a = num < 0 ? (num == INT64_MIN ? 0 : -num) : num;
    int64_t b = n2 < 0 ? (n2 == INT64_MIN ? 0 : -n2) : n2;
    if (a == 0 || b == 0) return false;
    const int64_t g1 = gcd_pos(a, d2);
    a /= g1;
    d2 /= g1;
    const int64_t g2 = gcd_pos(b, den);
    b /= g2;
    den /= g2;
    if (a > INT64_MAX / b || den > INT64_MAX / d2) return false;
    num = a * b;
    den = den * d2;
    const int64_t g = gcd_pos(num, den);
    num /= g;
    den /= g;
    if (neg) num = -num;
    return true;
}

inline bool act_mul(const CorpAct& act, PxMul& out) noexcept {
    out = PxMul{};
    if (act.jump_num <= 0 || act.jump_den <= 0) return false;
    out.num = act.jump_den;
    out.den = act.jump_num;
    if (act.cash_div <= 0 || !act.income) return true;
    if (act.prev_close <= act.cash_div) return false;
    return mul_frac(out.num, out.den, act.prev_close, act.prev_close - act.cash_div);
}

inline bool mul_fold(const CorpAct* acts, int n, int ymd, PxMul& out) noexcept {
    out = PxMul{};
    if (n < 0 || (n > 0 && !acts)) return false;
    for (int i = 0; i < n; ++i) {
        if (acts[i].ymd > ymd) continue;
        PxMul step;
        if (!act_mul(acts[i], step)) return false;
        if (!mul_frac(out.num, out.den, step.num, step.den)) return false;
    }
    return true;
}

inline bool scale_px(int64_t raw, PxMul mul, int64_t& out) noexcept {
    out = 0;
    if (raw < 0 || mul.den <= 0 || mul.num <= 0) return false;
    if (raw == 0) return true;
    Wide w = mul_wide(static_cast<uint64_t>(raw), static_cast<uint64_t>(mul.num));
    uint64_t q = 0;
    if (!div_wide(w, static_cast<uint64_t>(mul.den), q) || q > static_cast<uint64_t>(INT64_MAX)) return false;
    out = static_cast<int64_t>(q);
    return true;
}

// Traded move after removing the raw jump. num/den is ex/prev/jump - 1.
inline bool traded_move(int64_t prev, int64_t ex, int64_t jump_num, int64_t jump_den, int64_t& num, int64_t& den) noexcept {
    num = 0;
    den = 1;
    if (prev <= 0 || ex < 0 || jump_num <= 0 || jump_den <= 0) return false;
    if (ex > INT64_MAX / jump_den || prev > INT64_MAX / jump_num) return false;
    const int64_t left = ex * jump_den;
    const int64_t right = prev * jump_num;
    den = right;
    num = left - right;
    const int64_t g = gcd_pos(num, den);
    num /= g;
    den /= g;
    return true;
}

}  // namespace pitlane
