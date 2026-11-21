#pragma once

#include "pitlane/util/hint.hpp"
#include "pitlane/util/numeric.hpp"

#include <cstdint>

namespace pitlane {

// Smart order routing over a small, caller-owned venue snapshot. All arithmetic is integer and
// every path fails closed to zero slices. A fee is in basis points; a positive fee is a cost.

struct VenueTop {
    uint32_t venue;
    int64_t bid_px;
    int64_t ask_px;
    int64_t bid_qty;
    int64_t ask_qty;
    int32_t taker_fee_bps;
    int32_t maker_rebate_bps;
    int32_t passive_fill_ppm;
    int64_t venue_limit;
    int64_t lot;
};

struct SorSlice {
    uint32_t venue;
    int64_t qty;
    int64_t px;
};

// Price paid after a taker fee. A rebate lowers the buy price.
PITLANE_HOT PITLANE_INLINE int64_t eff_buy_px(int64_t px, int32_t fee_bps) noexcept {
    if (unlikely(px <= 0)) return 0;
    int64_t fee = static_cast<int64_t>(fee_bps);
    if (fee < -10000) fee = -10000;
    const int64_t num = 10000 + fee;
    if (num <= 0) return 0;
    if (px > INT64_MAX / num) return INT64_MAX;
    return (px * num) / 10000;
}

// Price received after a taker fee. A negative fee is treated as zero.
PITLANE_HOT PITLANE_INLINE int64_t eff_sell_px(int64_t px, int32_t fee_bps) noexcept {
    if (unlikely(px <= 0)) return 0;
    int64_t fee = static_cast<int64_t>(fee_bps);
    if (fee < 0) fee = 0;
    if (fee > 10000) fee = 10000;
    const int64_t num = 10000 - fee;
    if (num <= 0) return 0;
    if (px > INT64_MAX / num) return INT64_MAX;
    return (px * num) / 10000;
}

// Best net buy venues first. Each slice is capped by shown size and a positive venue limit,
// then rounded down to the venue lot. Returns the number of slices written.
template <int MaxVenues = 16>
int sor_aggressive_buy(const VenueTop* v, int n, int64_t want, SorSlice* out, int cap) noexcept {
    static_assert(MaxVenues >= 1 && MaxVenues <= 64, "sor venues");
    if (unlikely(!v || !out || n <= 0 || want <= 0 || cap <= 0)) return 0;
    VenueTop pick[MaxVenues];
    int m = 0;
    for (int i = 0; i < n && m < MaxVenues; ++i) {
        if (v[i].ask_px > 0 && v[i].ask_qty > 0) pick[m++] = v[i];
    }
    for (int i = 1; i < m; ++i) {
        const VenueTop key = pick[i];
        const int64_t kp = eff_buy_px(key.ask_px, key.taker_fee_bps);
        int j = i - 1;
        while (j >= 0) {
            const int64_t jp = eff_buy_px(pick[j].ask_px, pick[j].taker_fee_bps);
            if (jp < kp || (jp == kp && pick[j].venue < key.venue)) break;
            pick[j + 1] = pick[j];
            --j;
        }
        pick[j + 1] = key;
    }
    int64_t left = want;
    int k = 0;
    for (int i = 0; i < m && k < cap && left > 0; ++i) {
        int64_t take = pick[i].ask_qty;
        if (take > left) take = left;
        if (pick[i].venue_limit > 0 && take > pick[i].venue_limit) take = pick[i].venue_limit;
        const int64_t lot = pick[i].lot > 0 ? pick[i].lot : 1;
        take = (take / lot) * lot;
        if (take <= 0) continue;
        out[k].venue = pick[i].venue;
        out[k].qty = take;
        out[k].px = pick[i].ask_px;
        ++k;
        left -= take;
    }
    return k;
}

// Best net sell venues first. Symmetric with the buy path.
template <int MaxVenues = 16>
int sor_aggressive_sell(const VenueTop* v, int n, int64_t want, SorSlice* out, int cap) noexcept {
    static_assert(MaxVenues >= 1 && MaxVenues <= 64, "sor venues");
    if (unlikely(!v || !out || n <= 0 || want <= 0 || cap <= 0)) return 0;
    VenueTop pick[MaxVenues];
    int m = 0;
    for (int i = 0; i < n && m < MaxVenues; ++i) {
        if (v[i].bid_px > 0 && v[i].bid_qty > 0) pick[m++] = v[i];
    }
    for (int i = 1; i < m; ++i) {
        const VenueTop key = pick[i];
        const int64_t kp = eff_sell_px(key.bid_px, key.taker_fee_bps);
        int j = i - 1;
        while (j >= 0) {
            const int64_t jp = eff_sell_px(pick[j].bid_px, pick[j].taker_fee_bps);
            if (jp > kp || (jp == kp && pick[j].venue < key.venue)) break;
            pick[j + 1] = pick[j];
            --j;
        }
        pick[j + 1] = key;
    }
    int64_t left = want;
    int k = 0;
    for (int i = 0; i < m && k < cap && left > 0; ++i) {
        int64_t take = pick[i].bid_qty;
        if (take > left) take = left;
        if (pick[i].venue_limit > 0 && take > pick[i].venue_limit) take = pick[i].venue_limit;
        const int64_t lot = pick[i].lot > 0 ? pick[i].lot : 1;
        take = (take / lot) * lot;
        if (take <= 0) continue;
        out[k].venue = pick[i].venue;
        out[k].qty = take;
        out[k].px = pick[i].bid_px;
        ++k;
        left -= take;
    }
    return k;
}

// Passive allocation in proportion to fill probability times rebate. There is no side here, so a
// slice is priced at the resting bid when one is present and at the ask otherwise. The allocation
// is capped by a positive venue limit and rounded down to the lot.
template <int MaxVenues = 16>
int sor_passive(const VenueTop* v, int n, int64_t want, SorSlice* out, int cap) noexcept {
    static_assert(MaxVenues >= 1 && MaxVenues <= 64, "sor venues");
    if (unlikely(!v || !out || n <= 0 || want <= 0 || cap <= 0)) return 0;
    VenueTop pick[MaxVenues];
    int64_t weight[MaxVenues];
    int m = 0;
    int64_t total = 0;
    for (int i = 0; i < n && m < MaxVenues; ++i) {
        const int64_t ppm = v[i].passive_fill_ppm > 0 ? static_cast<int64_t>(v[i].passive_fill_ppm) : 0;
        const int64_t reb = v[i].maker_rebate_bps > 0 ? static_cast<int64_t>(v[i].maker_rebate_bps) : 0;
        const int64_t px = v[i].bid_px > 0 ? v[i].bid_px : v[i].ask_px;
        if (ppm == 0 || px <= 0) continue;
        if (ppm > INT64_MAX / (10000 + reb)) return 0;
        const int64_t w = ppm * (10000 + reb);
        if (total > INT64_MAX - w) return 0;
        pick[m] = v[i];
        weight[m] = w;
        total += w;
        ++m;
    }
    if (m == 0 || total <= 0) return 0;
    int64_t left = want;
    int k = 0;
    for (int i = 0; i < m && k < cap && left > 0; ++i) {
        const int64_t w = weight[i];
        if (w <= 0) continue;
        uint64_t q = 0;
        const Wide wide = mul_wide(static_cast<uint64_t>(left), static_cast<uint64_t>(w));
        const bool have = div_wide(wide, static_cast<uint64_t>(total), q);
        total -= w;
        if (!have || q > static_cast<uint64_t>(INT64_MAX)) continue;
        int64_t take = static_cast<int64_t>(q);
        if (pick[i].venue_limit > 0 && take > pick[i].venue_limit) take = pick[i].venue_limit;
        const int64_t lot = pick[i].lot > 0 ? pick[i].lot : 1;
        take = (take / lot) * lot;
        if (take <= 0) continue;
        out[k].venue = pick[i].venue;
        out[k].qty = take;
        out[k].px = pick[i].bid_px > 0 ? pick[i].bid_px : pick[i].ask_px;
        ++k;
        left -= take;
    }
    return k;
}

// Tracks the quantity already claimed against one shown size. A book stamp far from the mark
// starts a fresh booking period. release clamps at zero.
struct ShownHold {
    int32_t held{0};
    int64_t mark_us{0};
    int64_t fresh_us{0};

    PITLANE_HOT int32_t claim(int32_t want, int32_t shown, int64_t book_us) noexcept {
        if (want <= 0 || shown <= 0) return 0;
        const int64_t away = book_us >= mark_us ? book_us - mark_us : mark_us - book_us;
        if (away > fresh_us) {
            held = 0;
            mark_us = book_us;
        }
        int32_t room = shown - held;
        if (room > want) room = want;
        if (room <= 0) return 0;
        held += room;
        return room;
    }

    PITLANE_HOT void release(int32_t qty) noexcept {
        if (qty <= 0 || qty >= held) {
            held = 0;
            return;
        }
        held -= qty;
    }
};

}  // namespace pitlane
