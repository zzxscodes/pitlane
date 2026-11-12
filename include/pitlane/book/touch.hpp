#pragma once

#include "pitlane/book/arrive.hpp"
#include "pitlane/desk/types.hpp"

#include <cstdint>

namespace pitlane {

// Price resolve modes for paper / gate paths (simulator PriceType role).
enum class TouchMode : uint8_t {
    Opp = 1,   // opposite top (buy→ask, sell→bid)
    Own = 2,   // same-side top
    Last = 3,  // last trade
    Mid = 4,   // mid of bid/ask when both > 0
    Best5 = 5  // same as Opp for top; walk uses first 5 rungs
};

struct TouchView {
    int64_t bid_px{0};
    int64_t ask_px{0};
    int64_t last_px{0};
    const Rung* bids{nullptr};
    int nbid{0};
    const Rung* asks{nullptr};
    int nask{0};
};

inline int64_t resolve_px(bool buy, TouchMode mode, const TouchView& v) noexcept {
    switch (mode) {
        case TouchMode::Opp:
        case TouchMode::Best5:
            return buy ? v.ask_px : v.bid_px;
        case TouchMode::Own:
            return buy ? v.bid_px : v.ask_px;
        case TouchMode::Last:
            return v.last_px;
        case TouchMode::Mid:
            if (v.bid_px > 0 && v.ask_px > 0) return (v.bid_px + v.ask_px) / 2;
            return v.last_px;
        default:
            return 0;
    }
}

// Tape filter for rigid / same-direction skip (FilterExtTrades role).
inline bool tape_keep(uint8_t aggressor, uint8_t resting_side, bool rigid) noexcept {
    if (aggressor == 0) return true;
    if (aggressor == resting_side) return false;
    (void)rigid;
    return true;
}

} // namespace pitlane
