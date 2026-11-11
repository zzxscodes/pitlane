#pragma once

#include "pitlane/desk/types.hpp"
#include "pitlane/study/study.hpp"
#include "pitlane/util/hint.hpp"

// known_side lives in desk/types.hpp. Idea and BookTop live in study.hpp.

#include <cstdint>

namespace pitlane {

// Scratch helpers for a caller-owned decision. The library never chooses the side.
// The caller fills Idea, scores, and gates. These calls only store and compare numbers.

struct DecideScratch {
    Idea idea{};
    int64_t score{0};
    uint8_t armed{0};
};

PITLANE_INLINE void decide_clear(DecideScratch& d) noexcept {
    d.idea = Idea{};
    d.score = 0;
    d.armed = 0;
}

// Arms a candidate when qty and px are positive and the side is known.
PITLANE_INLINE bool decide_arm(DecideScratch& d, int64_t px, int64_t qty, uint8_t side, int64_t score) noexcept {
    if (unlikely(px <= 0 || qty <= 0 || !known_side(side))) return false;
    d.idea.px = px;
    d.idea.qty = qty;
    d.idea.side = side;
    d.score = score;
    d.armed = 1;
    return true;
}

// Keeps the candidate with the higher score. Equal score keeps the current one.
PITLANE_INLINE bool decide_prefer(DecideScratch& d, int64_t px, int64_t qty, uint8_t side, int64_t score) noexcept {
    if (unlikely(px <= 0 || qty <= 0 || !known_side(side))) return false;
    if (d.armed && score <= d.score) return false;
    return decide_arm(d, px, qty, side, score);
}

// Copies an armed idea into out. Does not send.
PITLANE_INLINE bool decide_take(const DecideScratch& d, Idea& out) noexcept {
    if (unlikely(!d.armed)) return false;
    out = d.idea;
    return true;
}

// Builds a buy or sell idea from a top of book the caller already holds.
// buy=true uses the ask. buy=false uses the bid. Missing size fails.
PITLANE_INLINE bool decide_from_top(const BookTop& top, bool buy, int64_t qty, Idea& out) noexcept {
    if (unlikely(qty <= 0)) return false;
    if (buy) {
        if (unlikely(top.ask_px <= 0 || top.ask_qty <= 0)) return false;
        out.px = top.ask_px;
        out.qty = qty;
        out.side = 1;
        return true;
    }
    if (unlikely(top.bid_px <= 0 || top.bid_qty <= 0)) return false;
    out.px = top.bid_px;
    out.qty = qty;
    out.side = 2;
    return true;
}

}  // namespace pitlane
