#pragma once

#include <cstdint>

namespace pitlane {

// Differences of timestamps the caller already stored.
// A negative stamp is unset. The library does not read a NIC or a socket.

inline bool lap_ns(int64_t from_ns, int64_t to_ns, int64_t& out) noexcept {
    if (from_ns < 0 || to_ns < 0 || to_ns < from_ns) return false;
    out = to_ns - from_ns;
    return true;
}

inline bool hop_ns(int64_t trigger_ns, int64_t gen_ns, int64_t& out) noexcept {
    return lap_ns(trigger_ns, gen_ns, out);
}

inline bool order_to_ack(int64_t send_ns, int64_t live_ns, int64_t& out) noexcept {
    return lap_ns(send_ns, live_ns, out);
}

inline bool tick_to_trade(int64_t tick_ns, int64_t live_ns, int64_t& out) noexcept {
    return lap_ns(tick_ns, live_ns, out);
}

inline bool tick_to_cancel(int64_t tick_ns, int64_t cancel_ns, int64_t& out) noexcept {
    return lap_ns(tick_ns, cancel_ns, out);
}

}  // namespace pitlane
