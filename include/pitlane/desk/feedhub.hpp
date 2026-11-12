#pragma once

#include "pitlane/desk/feed.hpp"
#include "pitlane/desk/region.hpp"
#include "pitlane/desk/yard.hpp"
#include "pitlane/desk/wire.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Local MD framework hub (SimMarketData::ProcessQuote / WriteQuote role).
// Caller owns sockets; hub only fans into Yard + Desk rings. Hot path no alloc.

struct FeedHub {
    WantMask* want{nullptr};
    YardView* yard{nullptr};
    Desk* desk{nullptr};
    unsigned quote_mask{0}; // FanRing reader mask; 0 = first acker
    unsigned print_mask{0};
};

PITLANE_HOT inline bool feed_push_mark(FeedHub& hub, uint32_t sym_id, const YardMark& mark,
                                       const Quote* desk_quote = nullptr) noexcept {
    if (hub.want && !hub.want->accept(sym_id)) return false;
    if (hub.yard) {
        if (!yard_publish(*hub.yard, sym_id, mark)) return false;
    }
    if (hub.desk && desk_quote) {
        if (!hub.desk->quotes.publish(*desk_quote, hub.quote_mask)) return false;
    }
    return true;
}

PITLANE_HOT inline bool feed_push_print(FeedHub& hub, uint32_t sym_id, const YardPrint& print,
                                        const TapePrint* desk_print = nullptr) noexcept {
    if (hub.want && !hub.want->accept(sym_id)) return false;
    if (hub.yard) {
        if (!yard_publish_print(*hub.yard, sym_id, print)) return false;
    }
    if (hub.desk && desk_print) {
        if (!hub.desk->prints.publish(*desk_print, hub.print_mask)) return false;
    }
    return true;
}

PITLANE_HOT inline bool feed_push_tick(FeedHub& hub, uint32_t sym_id, const YardTick& tick) noexcept {
    if (hub.want && !hub.want->accept(sym_id)) return false;
    if (!hub.yard) return false;
    return yard_publish_tick(*hub.yard, sym_id, tick);
}

} // namespace pitlane
