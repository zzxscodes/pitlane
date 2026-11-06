#pragma once

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

}  // namespace pitlane
