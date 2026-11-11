#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// One venue top of book on its own cache line. The strategy keeps local copies.
// Feeding a quote updates one side. The helpers do not decide a trade.

struct alignas(kCacheLine) TopQuote {
    int64_t bid_px{0};
    int64_t ask_px{0};
    int32_t bid_sz{0};
    int32_t ask_sz{0};
    uint64_t src_seq{0};
    uint8_t venue{0};
    uint8_t live{0};
};

static_assert(sizeof(TopQuote) == kCacheLine, "top quote occupies one line");

struct OwnPair {
    TopQuote a{};
    TopQuote b{};
    uint8_t a_ok{0};
    uint8_t b_ok{0};
    uint8_t want_a{0};
    uint8_t want_b{0};

    PITLANE_INLINE void feed(const TopQuote& q) noexcept {
        if (q.venue == want_a) {
            a = q;
            a_ok = 1;
        } else if (q.venue == want_b) {
            b = q;
            b_ok = 1;
        }
    }

    PITLANE_INLINE bool ready() const noexcept { return a_ok != 0 && b_ok != 0 && a.live != 0 && b.live != 0; }

    // Positive when a's bid is above b's ask. Negative when b's bid is above a's ask. Else 0.
    PITLANE_INLINE int64_t cross_ticks() const noexcept {
        if (!ready()) return 0;
        if (a.bid_px > b.ask_px) return a.bid_px - b.ask_px;
        if (b.bid_px > a.ask_px) return -(b.bid_px - a.ask_px);
        return 0;
    }
};

}  // namespace pitlane
