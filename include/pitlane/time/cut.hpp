#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Whether one core should keep the whole path, or the caller should split work.
// The numbers are supplied by the caller. The helper does not start a thread.

enum class CutChoice : uint8_t { KeepSolo = 0, SplitStateful = 1, FanoutStateless = 2 };

PITLANE_INLINE CutChoice pick_cut(uint64_t stage_p99_ns, uint64_t gap_p01_ns, bool stateful, int cores) noexcept {
    if (stage_p99_ns <= gap_p01_ns) return CutChoice::KeepSolo;
    if (!stateful && cores >= 4) return CutChoice::FanoutStateless;
    return CutChoice::SplitStateful;
}

// One packet's finish time through n equal stages with handoff h between stages.
// arrivals are absolute times. out[i] is latency for packet i. Cap is the buffer length.
PITLANE_HOT inline int tandem_lat(const uint64_t* __restrict arrivals, int n_pkt, uint64_t service, uint64_t stages,
                                  uint64_t handoff, uint64_t* __restrict out, int cap) noexcept {
    if (unlikely(!arrivals || !out || n_pkt <= 0 || cap < n_pkt || stages == 0 || service < stages)) return 0;
    uint64_t dep[256];
    if (unlikely(n_pkt > 256)) return 0;
    for (int i = 0; i < n_pkt; ++i) dep[i] = arrivals[i];
    const uint64_t slice = service / stages;
    for (uint64_t stage = 0; stage < stages; ++stage) {
        uint64_t free_at = 0;
        for (int i = 0; i < n_pkt; ++i) {
            const uint64_t arrive = stage == 0 ? arrivals[i] : dep[i] + handoff;
            free_at = (arrive > free_at ? arrive : free_at) + slice;
            dep[i] = free_at;
        }
    }
    for (int i = 0; i < n_pkt; ++i) out[i] = dep[i] - arrivals[i];
    return n_pkt;
}

// Continuous sessions only. A gap is skipped; a start inside a gap jumps to the next open.
struct SessionSpan {
    int64_t begin_us{0};
    int64_t end_us{0};
};

inline int64_t add_session_us(int64_t t, int64_t dur, const SessionSpan* cont, int n) noexcept {
    if (dur <= 0 || n <= 0 || !cont) return t;
    int i = 0;
    while (i < n && t >= cont[i].end_us) ++i;
    if (i == n) return cont[n - 1].end_us;
    if (t < cont[i].begin_us) t = cont[i].begin_us;
    while (dur > 0 && i < n) {
        const int64_t room = cont[i].end_us - t;
        if (room < 0) return t;
        if (dur <= room) return t + dur;
        dur -= room;
        if (++i == n) return cont[n - 1].end_us;
        t = cont[i].begin_us;
    }
    return t;
}

}  // namespace pitlane
