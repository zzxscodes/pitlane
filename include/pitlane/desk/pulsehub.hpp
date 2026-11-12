#pragma once

#include "pitlane/desk/region.hpp"
#include "pitlane/desk/signal.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Drain PulseBoard into desk.pulses (local stand-in for a messaging hub fan-out).
// Hot path: no alloc. Returns number published; stops early if the ring is full.

PITLANE_HOT inline int pulse_bridge(PulseBoard& board, Desk& desk, unsigned mask = 0) noexcept {
    Pulse scratch[32]{};
    int total = 0;
    for (;;) {
        const int n = board.drain(scratch, 32);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            if (!desk.pulses.publish(scratch[i], mask)) return total;
            ++total;
        }
        if (n < 32) break;
    }
    return total;
}

} // namespace pitlane
