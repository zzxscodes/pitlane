#pragma once

#include <cstdint>

namespace pitlane {

struct LinearBook {
    int64_t mult{1};
    int64_t cash{0};
    int64_t pos{0};
    int64_t fee{0};

    int64_t notion(int64_t px, int64_t qty) const noexcept { return mult * px * qty; }
    int64_t equity(int64_t px) const noexcept { return cash + mult * pos * px - fee; }
};

struct InverseBook {
    int64_t mult{1};
    int64_t cash{0};
    int64_t pos{0};
    int64_t fee{0};

    int64_t notion(int64_t px, int64_t qty) const noexcept { return px == 0 ? 0 : mult * qty / px; }
    int64_t equity(int64_t px) const noexcept { return px == 0 ? 0 : -cash - (mult * pos) / px - fee; }
};

// Rates are in 1e-9 of the base (a rate of 1e6 is 0.001 when applied to an integer notion
// after the caller scales). This helper stays in integers: rate_num / rate_den.
inline int64_t charge_fee(int64_t notion, int64_t qty, bool rested, bool buy, bool by_qty, int64_t rest_num,
                          int64_t take_num, int64_t buy_num, int64_t sell_num, int64_t den) noexcept {
    if (den <= 0) return 0;
    const int64_t side = buy ? buy_num : sell_num;
    const int64_t leg = rested ? rest_num : take_num;
    if (by_qty) return (leg * qty) / den + (side * notion) / den;
    return ((leg + side) * notion) / den;
}

inline void apply_fill(LinearBook& b, bool buy, int64_t px, int64_t qty, int64_t fee) noexcept {
    const int64_t n = b.notion(px, qty);
    if (buy) {
        b.pos += qty;
        b.cash -= n;
    } else {
        b.pos -= qty;
        b.cash += n;
    }
    b.fee += fee;
}

}  // namespace pitlane
