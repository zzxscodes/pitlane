#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Request sequence is monotonic (idempotency via client id). Report sequence is
// contiguous (gap detect). Heartbeat carries the peer watermark. Times are µs.

struct DualLane {
    uint64_t req_sent{0};
    uint64_t rpt_applied{0};
    uint64_t peer_seen{0};
    uint64_t hold_lo{0};
    uint64_t hold_hi{0};
    int64_t beat_us{0};
    int64_t gap_us{2000000};  // two seconds of silence ends the session by default
};

PITLANE_INLINE uint64_t next_req(DualLane& d) noexcept {
    if (unlikely(d.req_sent == UINT64_MAX)) return 0;
    ++d.req_sent;
    return d.req_sent;
}

// 0 duplicate / already applied, 1 contiguous live, 2 gap.
PITLANE_INLINE int on_report(DualLane& d, uint64_t seq) noexcept {
    if (seq == 0 || seq <= d.rpt_applied) return 0;
    if (seq == d.rpt_applied + 1u) {
        d.rpt_applied = seq;
        return 1;
    }
    return 2;
}

PITLANE_INLINE uint64_t sync_from(const DualLane& d) noexcept {
    const uint64_t a = d.rpt_applied + 1u;
    return a < d.hold_lo ? d.hold_lo : a;
}

PITLANE_INLINE bool window_ok(const DualLane& d) noexcept { return d.rpt_applied + 1u >= d.hold_lo; }

PITLANE_INLINE void on_beat_lane(DualLane& d, uint64_t peer, int64_t now_us) noexcept {
    d.peer_seen = peer;
    d.beat_us = now_us;
}

PITLANE_INLINE bool lane_dead(const DualLane& d, int64_t now_us) noexcept {
    if (d.beat_us == 0) return false;
    return now_us - d.beat_us >= d.gap_us;
}

PITLANE_INLINE void day_cut(DualLane& d) noexcept {
    d.req_sent = 0;
    d.rpt_applied = 0;
    d.peer_seen = 0;
    d.beat_us = 0;
}

}  // namespace pitlane
