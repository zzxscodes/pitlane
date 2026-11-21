#pragma once

#include "pitlane/desk/broker.hpp"
#include "pitlane/order/ack.hpp"
#include "pitlane/order/idclaim.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// List-only scan of timed-out inflight tags. Does not send I/O and does not drop BrokerWatch rows.
// AckBook::sweep lists due locals without clearing ack rows. IdClaim Live ids are appended when
// claim_all_live is set; otherwise Live ids are kept only when they already appear in due/sweep.

struct InflightScan {
    int64_t now_us{0};
    int64_t ack_limit_us{0};
    int64_t life_limit_us{0};
    int64_t pull_limit_us{0};
    AckClock* ack_clock{nullptr};
    uint8_t claim_all_live{0};
};

namespace detail {

PITLANE_INLINE bool push_unique(uint64_t* out, int& n, int cap, uint64_t tag) noexcept {
    if (tag == 0) return true;
    for (int i = 0; i < n; ++i) {
        if (out[i] == tag) return true;
    }
    if (n >= cap) return false;
    out[n++] = tag;
    return true;
}

}  // namespace detail

// Returns written count, or -1 when the output cannot hold the full unique set (fail-closed).
template <int ClaimCap, int AckCap>
inline int inflight_scan(BrokerWatch& watch, IdClaim<ClaimCap>* claim, AckBook<AckCap>* acks, InflightScan opt,
                         uint64_t* out, int cap) noexcept {
    if (!out || cap <= 0 || opt.now_us < 0) return -1;
    uint64_t buf[BrokerWatch::kCap];
    const int due_n = watch.due(opt.now_us, opt.ack_limit_us, opt.life_limit_us, opt.pull_limit_us, buf, BrokerWatch::kCap);
    if (due_n < 0) return -1;
    int n = 0;
    for (int i = 0; i < due_n; ++i) {
        if (!detail::push_unique(out, n, cap, buf[i])) return -1;
    }
    if (acks && opt.ack_clock) {
        uint64_t ack_due[AckCap];
        const int an = acks->sweep(*opt.ack_clock, opt.now_us, ack_due, AckCap);
        if (an < 0) return -1;
        for (int i = 0; i < an; ++i) {
            if (!detail::push_unique(out, n, cap, ack_due[i])) return -1;
        }
    }
    if (claim && opt.claim_all_live) {
        for (int i = 0; i < ClaimCap; ++i) {
            if (claim->kind_at(i) != IdKind::Live) continue;
            if (!detail::push_unique(out, n, cap, claim->id_at(i))) return -1;
        }
    }
    return n;
}

}  // namespace pitlane
