#pragma once

#include "pitlane/book/drill.hpp"
#include "pitlane/book/venue.hpp"
#include "pitlane/desk/region.hpp"
#include "pitlane/desk/runtime.hpp"
#include "pitlane/desk/yard.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Local TD match lane (SimTrader::LoopTask / InstrumentMatcher role).
// Drains desk intents into PaperVenue and publishes reports. Hot path no alloc.
// When VenueCfg.tape_first (Match.use_ext), prints are applied before intents.

struct TradeLane {
    PaperVenue* venue{nullptr};
    DrillBook* drills{nullptr};
    YardView* yard{nullptr};
    unsigned intent_reader{kLoopReader};
    unsigned report_mask{0};
    uint64_t intent_cur{0};
    uint64_t print_cur{0};
    Report scratch[32]{};
};

namespace detail {

PITLANE_HOT inline int lane_drain_prints(TradeLane& lane, Desk& desk, int64_t now_us) noexcept {
    int events = 0;
    PaperVenue& v = *lane.venue;
    TapePrint pr{};
    bool pf = false;
    while (desk.prints.at(lane.print_cur, lane.intent_reader, pr, &pf)) {
        if (!pf) break;
        if (lane.yard && pr.name != 0) {
            YardMark m{};
            if (yard_latest(*lane.yard, pr.name, m)) {
                v.set_top(pr.name, m.bid_px, m.bid_qty, m.ask_px, m.ask_qty, pr.px);
            }
        }
        const int tn = v.on_tape(pr.name, pr.px, pr.qty, pr.aggressor, now_us, lane.scratch, 32);
        for (int i = 0; i < tn; ++i) {
            if (!desk.reports.publish(lane.scratch[i], lane.report_mask)) break;
            ++events;
        }
        desk.prints.ack(lane.print_cur, lane.intent_reader);
        ++lane.print_cur;
    }
    return events;
}

} // namespace detail

PITLANE_HOT inline int lane_beat(TradeLane& lane, Desk& desk, int64_t now_us) noexcept {
    if (!lane.venue) return -1;
    int events = 0;
    PaperVenue& v = *lane.venue;

    if (v.tape_first()) events += detail::lane_drain_prints(lane, desk, now_us);

    Intent in{};
    bool fresh = false;
    while (desk.intents.at(lane.intent_cur, lane.intent_reader, in, &fresh)) {
        if (!fresh) break;
        const DrillRow* drill = lane.drills ? lane.drills->find(in.tag) : nullptr;
        if (drill && drill->cue == DrillCue::Closed) {
            (void)v.drill_apply(*drill, now_us, lane.scratch, 32);
            if (in.act == static_cast<uint8_t>(Act::Enter)) {
                desk.intents.ack(lane.intent_cur, lane.intent_reader);
                ++lane.intent_cur;
                continue;
            }
        }
        const VenueOut out = v.admit(in, now_us, lane.scratch, 32);
        for (int i = 0; i < out.reports; ++i) {
            if (!desk.reports.publish(lane.scratch[i], lane.report_mask)) break;
            ++events;
        }
        if (drill && drill->cue != DrillCue::Closed) {
            const int dn = v.drill_apply(*drill, now_us, lane.scratch, 32);
            for (int i = 0; i < dn; ++i) {
                if (!desk.reports.publish(lane.scratch[i], lane.report_mask)) break;
                ++events;
            }
        }
        desk.intents.ack(lane.intent_cur, lane.intent_reader);
        ++lane.intent_cur;
    }

    const int clock_n = v.on_clock(now_us, lane.scratch, 32);
    for (int i = 0; i < clock_n; ++i) {
        if (!desk.reports.publish(lane.scratch[i], lane.report_mask)) break;
        ++events;
    }

    if (!v.tape_first()) events += detail::lane_drain_prints(lane, desk, now_us);

    return events;
}

} // namespace pitlane
