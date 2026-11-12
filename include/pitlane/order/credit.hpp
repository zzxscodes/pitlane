#pragma once

#include "pitlane/order/lots.hpp"

#include <cstdint>

namespace pitlane {

// Short inventory rebuilt from a liability tape, plus a special-purpose pool (notes §51).
// Option free size after combined-lot deduction (notes §50). Integer only.

enum class CreditPool : uint8_t { Normal = 1, Special = 2 };

struct CreditCash {
    int64_t long_qty{0};
    int64_t short_qty{0};
    int64_t special_short{0};  // dedicated short pool, not mixed into normal
};

inline int64_t credit_from_long(const CreditCash& c) noexcept {
    return c.long_qty > 0 ? c.long_qty : 0;
}

inline int64_t credit_from_short(const CreditCash& c) noexcept {
    const int64_t n = c.short_qty > 0 ? c.short_qty : 0;
    const int64_t s = c.special_short > 0 ? c.special_short : 0;
    if (n > INT64_MAX - s) return INT64_MAX;
    return n + s;
}

inline CreditPool map_credit(bool special) noexcept { return special ? CreditPool::Special : CreditPool::Normal; }

// Rebuild short from signed liability deltas (positive increases short debt).
inline bool credit_apply_liability(CreditCash& c, int64_t delta, bool special) noexcept {
    if (delta == 0) return true;
    int64_t& dst = special ? c.special_short : c.short_qty;
    if (delta > 0) {
        if (dst > INT64_MAX - delta) return false;
        dst += delta;
        return true;
    }
    const int64_t mag = -delta;
    if (dst < mag) return false;
    dst -= mag;
    return true;
}

inline bool credit_short_ok(const CreditCash& c, int64_t qty, bool special) noexcept {
    if (qty <= 0) return false;
    if (special) return c.special_short >= qty;
    return c.short_qty >= qty;
}

inline bool credit_to_margin(const CreditCash& c, MarginBook& m) noexcept {
    m.short_avail = credit_from_short(c);
    return true;
}

// Option free size: long and short nets after combined (combo) lots are reserved.
inline int64_t option_free(int64_t long_qty, int64_t short_qty, int64_t combo) noexcept {
    if (combo < 0) combo = 0;
    int64_t L = long_qty > 0 ? long_qty : 0;
    int64_t S = short_qty > 0 ? short_qty : 0;
    if (combo > 0) {
        const int64_t take = L < combo ? L : combo;
        L -= take;
        combo -= take;
    }
    if (combo > 0) {
        const int64_t take = S < combo ? S : combo;
        S -= take;
    }
    if (L > INT64_MAX - S) return INT64_MAX;
    return L + S;
}

}  // namespace pitlane
