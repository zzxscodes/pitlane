#pragma once

#include "pitlane/desk/types.hpp"

#include <cstdint>

namespace pitlane {

struct Rung {
    int64_t px{0};
    int64_t qty{0};
};

enum class Arrival : uint8_t { Rest = 1, Take = 2, Drop = 3 };

// Maker-only is Style::Passive: any cross drops. Alias helper for call sites.
inline bool maker_only_style(Style style) noexcept { return style == Style::Passive; }

struct ArrivalHit {
    Arrival kind{Arrival::Drop};
    int64_t filled{0};
};

// asks[0] is the best (lowest) ask. Buys trade through rungs at or below the limit.
// A day order fills its whole remainder once it reaches its limit, even past shown size.
// An immediate order keeps only shown size and drops the rest.
// An all-or-nothing order drops the entire order when shown size is short.
// A passive order drops as soon as it would cross.

inline ArrivalHit walk_buy(Style style, int64_t px, int64_t leaves, const Rung* asks, int n) noexcept {
    if (leaves <= 0) return {Arrival::Drop, 0};
    const bool cross = n > 0 && asks && asks[0].px <= px;
    if (!cross) {
        if (style == Style::Immediate || style == Style::All) return {Arrival::Drop, 0};
        return {Arrival::Rest, 0};
    }
    if (style == Style::Passive) return {Arrival::Drop, 0};

    if (style == Style::All) {
        int64_t sum = 0;
        for (int i = 0; i < n; ++i) {
            if (asks[i].px > px) break;
            sum += asks[i].qty;
        }
        if (sum < leaves) return {Arrival::Drop, 0};
    }

    int64_t left = leaves;
    int64_t got = 0;
    for (int i = 0; i < n && left > 0; ++i) {
        if (asks[i].px > px) break;
        if (asks[i].px < px) {
            int64_t take = asks[i].qty;
            if (take > left) take = left;
            got += take;
            left -= take;
            continue;
        }
        if (style == Style::Day) {
            got += left;
            left = 0;
            break;
        }
        int64_t take = asks[i].qty;
        if (take > left) take = left;
        got += take;
        left -= take;
    }
    if (style == Style::Day && left > 0) {
        got += left;
        left = 0;
    }
    if (left > 0) return {Arrival::Drop, got};
    return {Arrival::Take, got};
}

// bids[0] is the best (highest) bid.
inline ArrivalHit walk_sell(Style style, int64_t px, int64_t leaves, const Rung* bids, int n) noexcept {
    if (leaves <= 0) return {Arrival::Drop, 0};
    const bool cross = n > 0 && bids && bids[0].px >= px;
    if (!cross) {
        if (style == Style::Immediate || style == Style::All) return {Arrival::Drop, 0};
        return {Arrival::Rest, 0};
    }
    if (style == Style::Passive) return {Arrival::Drop, 0};

    if (style == Style::All) {
        int64_t sum = 0;
        for (int i = 0; i < n; ++i) {
            if (bids[i].px < px) break;
            sum += bids[i].qty;
        }
        if (sum < leaves) return {Arrival::Drop, 0};
    }

    int64_t left = leaves;
    int64_t got = 0;
    for (int i = 0; i < n && left > 0; ++i) {
        if (bids[i].px < px) break;
        if (bids[i].px > px) {
            int64_t take = bids[i].qty;
            if (take > left) take = left;
            got += take;
            left -= take;
            continue;
        }
        if (style == Style::Day) {
            got += left;
            left = 0;
            break;
        }
        int64_t take = bids[i].qty;
        if (take > left) take = left;
        got += take;
        left -= take;
    }
    if (style == Style::Day && left > 0) {
        got += left;
        left = 0;
    }
    if (left > 0) return {Arrival::Drop, got};
    return {Arrival::Take, got};
}

// Touch-only path: a cross fills the whole remainder at the best price.
// Shown size at that price is ignored. Empty book does not cross.
inline ArrivalHit touch_buy(Style style, int64_t px, int64_t leaves, int64_t best_ask, bool have_ask) noexcept {
    if (leaves <= 0) return {Arrival::Drop, 0};
    const bool cross = have_ask && px >= best_ask;
    if (!cross) {
        if (style == Style::Immediate || style == Style::All) return {Arrival::Drop, 0};
        return {Arrival::Rest, 0};
    }
    if (style == Style::Passive) return {Arrival::Drop, 0};
    return {Arrival::Take, leaves};
}

inline ArrivalHit touch_sell(Style style, int64_t px, int64_t leaves, int64_t best_bid, bool have_bid) noexcept {
    if (leaves <= 0) return {Arrival::Drop, 0};
    const bool cross = have_bid && px <= best_bid;
    if (!cross) {
        if (style == Style::Immediate || style == Style::All) return {Arrival::Drop, 0};
        return {Arrival::Rest, 0};
    }
    if (style == Style::Passive) return {Arrival::Drop, 0};
    return {Arrival::Take, leaves};
}

// Three-number median used when a paper fill has only a limit, a touch, and a last price.
inline int64_t median3(int64_t a, int64_t b, int64_t c) noexcept {
    if (a > b) {
        const int64_t t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        const int64_t t = b;
        b = c;
        c = t;
    }
    if (a > b) {
        const int64_t t = a;
        a = b;
        b = t;
    }
    return b;
}

}  // namespace pitlane
