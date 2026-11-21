#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Adaptive micro-batch sizing from queue backlog.
// Low backlog -> small batches (latency). High backlog -> larger batches (throughput).

struct BatchPolicy {
    uint32_t min{1};
    uint32_t normal{32};
    uint32_t max{256};
    uint32_t low_backlog{8};
    uint32_t high_backlog{64};
};

PITLANE_HOT PITLANE_INLINE uint32_t batch_size_for(uint32_t backlog, BatchPolicy pol = {}) noexcept {
    if (pol.min == 0) pol.min = 1;
    if (pol.normal < pol.min) pol.normal = pol.min;
    if (pol.max < pol.normal) pol.max = pol.normal;
    if (pol.high_backlog < pol.low_backlog) pol.high_backlog = pol.low_backlog;
    if (backlog <= pol.low_backlog) return pol.min;
    if (backlog <= pol.high_backlog) return pol.normal;
    return pol.max;
}

// Caps the adaptive size by how many slots are free in a fixed-capacity ring.
PITLANE_HOT PITLANE_INLINE uint32_t batch_size_capped(uint32_t backlog, uint32_t free_slots,
                                                     BatchPolicy pol = {}) noexcept {
    const uint32_t want = batch_size_for(backlog, pol);
    if (free_slots == 0) return 0;
    return want < free_slots ? want : free_slots;
}

}  // namespace pitlane
