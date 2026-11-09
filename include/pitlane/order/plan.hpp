#pragma once

#include "pitlane/util/numeric.hpp"
#include "pitlane/book/steps.hpp"

#include <cstdint>

namespace pitlane {

enum class MoveKind : uint8_t {
    None = 0,
    BuyOpen,
    SellOpen,
    BuyClose,
    SellClose,
    BuyCloseToday,
    SellCloseToday,
    BuyCloseYday,
    SellCloseYday
};

struct NetHold {
    int64_t long_today{0};
    int64_t long_yday{0};
    int64_t short_today{0};
    int64_t short_yday{0};
};

struct Move {
    MoveKind kind{MoveKind::None};
    int64_t qty{0};
    int64_t px{0};
};

inline int64_t long_sum(const NetHold& h) noexcept { return h.long_today + h.long_yday; }
inline int64_t short_sum(const NetHold& h) noexcept { return h.short_today + h.short_yday; }

// One working order at a time. Positive target wants a long position.
// When today and yesterday are split, yesterday is closed before today.
// Opening a long uses the ask. Opening a short uses the bid.

inline Move one_move(int64_t target, const NetHold& h, int64_t bid_px, int64_t ask_px, bool split_yday) noexcept {
    auto close_long = [&](int64_t excess) -> Move {
        if (!split_yday) return {MoveKind::SellClose, excess, bid_px};
        if (h.long_yday > 0) {
            const int64_t q = excess < h.long_yday ? excess : h.long_yday;
            return {MoveKind::SellCloseYday, q, bid_px};
        }
        return {MoveKind::SellCloseToday, excess, bid_px};
    };
    auto close_short = [&]() -> Move {
        if (!split_yday) return {MoveKind::BuyClose, short_sum(h), ask_px};
        if (h.short_yday > 0) return {MoveKind::BuyCloseYday, h.short_yday, ask_px};
        return {MoveKind::BuyCloseToday, h.short_today, ask_px};
    };

    if (target > 0) {
        if (short_sum(h) > 0) return close_short();
        const int64_t have = long_sum(h);
        if (have > target) return close_long(have - target);
        if (have < target) return {MoveKind::BuyOpen, target - have, ask_px};
        return {};
    }
    if (target < 0) {
        if (long_sum(h) > 0) return close_long(long_sum(h));
        const int64_t need = -target;
        const int64_t have = short_sum(h);
        if (have > need) {
            const int64_t excess = have - need;
            if (!split_yday) return {MoveKind::SellClose, excess, bid_px};
            if (h.short_yday > 0) {
                const int64_t q = excess < h.short_yday ? excess : h.short_yday;
                return {MoveKind::SellCloseYday, q, bid_px};
            }
            return {MoveKind::SellCloseToday, excess, bid_px};
        }
        if (have < need) return {MoveKind::SellOpen, need - have, bid_px};
        return {};
    }
    if (long_sum(h) > 0) return close_long(long_sum(h));
    if (short_sum(h) > 0) return close_short();
    return {};
}

inline int64_t rest_room(bool hit_together, int64_t cap, int64_t goal, int64_t weight, int64_t net_pos,
                         int64_t resting) noexcept {
    if (cap < 0) return 0;
    if (hit_together) return 0;
    const int64_t w = weight < 0 ? -weight : weight;
    const int64_t net = net_pos < 0 ? -net_pos : net_pos;
    if (goal < 0 || resting < 0) return 0;
    if (cmp_prod(w, goal, INT64_MAX, 1) > 0) return cap;
    const int64_t raw = w * goal;
    if (net > INT64_MAX - resting) return 0;
    if (raw < net + resting) return 0;
    const int64_t room = raw - net - resting;
    return room < cap ? room : cap;
}

inline void cut_hedge(int64_t& kept, int64_t& other, int64_t left) noexcept {
    const int64_t before = kept >= 0 ? kept : -kept;
    const int64_t after = left >= 0 ? left : -left;
    kept = left;
    if (after >= before) return;
    const int64_t mag = before - after;
    if (other >= mag) other -= mag;
    else if (other <= -mag) other += mag;
    else other = 0;
}

inline bool slice_weight(int64_t num, int64_t den, int64_t weight, int64_t lot_cap, int64_t filled,
                         int64_t& out) noexcept {
    out = 0;
    if (den <= 0 || lot_cap < 0 || num < 0) return false;
    const int64_t mag_w = weight < 0 ? -weight : weight;
    Wide w = mul_wide(static_cast<uint64_t>(mag_w), static_cast<uint64_t>(num));
    const uint64_t half = static_cast<uint64_t>(den / 2);
    if (w.lo > UINT64_MAX - half) {
        if (w.hi == UINT64_MAX) return false;
        ++w.hi;
        w.lo += half;
    } else {
        w.lo += half;
    }
    uint64_t rounded = 0;
    if (!div_wide(w, static_cast<uint64_t>(den), rounded)) return false;
    if (rounded > static_cast<uint64_t>(lot_cap)) rounded = static_cast<uint64_t>(lot_cap);
    const int64_t signed_qty = weight < 0 ? -static_cast<int64_t>(rounded) : static_cast<int64_t>(rounded);
    out = signed_qty - filled;
    return out > 0;
}

inline int64_t align_leg(int64_t raw, int64_t feed, int64_t tick) noexcept {
    if (raw <= 0 || feed <= 0 || tick <= 0) return 0;
    if (feed > INT64_MAX / tick) return INT64_MAX;
    const int64_t unit = feed * tick;
    const int64_t steps = raw / unit;
    const int64_t rem = raw % unit;
    const bool up = rem > (INT64_MAX / 2) || rem * 2 >= unit;
    const int64_t n = up ? steps + 1 : steps;
    if (n > INT64_MAX / tick) return INT64_MAX;
    return n * tick;
}

inline bool lead_code(const char* code, const char* prefix) noexcept {
    if (!code || !prefix) return false;
    while (*prefix) {
        if (*code++ != *prefix++) return false;
    }
    return true;
}

inline bool pair_qty_ok(bool sh_or_sz, const char* code, int64_t qty) noexcept {
    if (qty <= 0) return false;
    if (!sh_or_sz) return true;
    if (lead_code(code, "688") || lead_code(code, "689")) return qty >= 180;
    if (lead_code(code, "1000") || lead_code(code, "9000")) return true;
    return qty >= 80;
}

inline int64_t chase_px(bool buy, int64_t px, int ticks, const PxBand* bands, int n, int64_t down,
                        int64_t up) noexcept {
    return buy ? walk_up(bands, n, px, ticks, down, up) : walk_down(bands, n, px, ticks, down, up);
}

}  // namespace pitlane
