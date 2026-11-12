#pragma once

#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstdint>

namespace pitlane {

// Wait-free sticky counter from the notes. High bits carry is_zero / helped.
// decrement returns true when this call is the logical transition to zero.
// increment_if_not_zero fails once the counter is sticky-zero.

class StickyCounter {
public:
    static constexpr uint64_t kIsZero = 1ull << 63;
    static constexpr uint64_t kHelped = 1ull << 62;
    static constexpr uint64_t kMask = ~(kIsZero | kHelped);

    explicit StickyCounter(uint64_t initial = 1) noexcept : counter_(initial & kMask) {}

    PITLANE_HOT uint64_t read() noexcept {
        uint64_t val = counter_.load(std::memory_order_acquire);
        if (val == 0) {
            if (counter_.compare_exchange_strong(val, kIsZero | kHelped, std::memory_order_acq_rel)) return 0;
        }
        return (val & kIsZero) ? 0 : (val & kMask);
    }

    PITLANE_HOT bool decrement() noexcept {
        if (counter_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            uint64_t expect = 0;
            if (counter_.compare_exchange_strong(expect, kIsZero, std::memory_order_acq_rel)) return true;
            if ((expect & kHelped) != 0 && (counter_.exchange(kIsZero, std::memory_order_acq_rel) & kHelped) != 0) {
                return true;
            }
        }
        return false;
    }

    PITLANE_HOT bool increment_if_not_zero() noexcept {
        return (counter_.fetch_add(1, std::memory_order_acq_rel) & kIsZero) == 0;
    }

private:
    alignas(kCacheLine) std::atomic<uint64_t> counter_;
};

}  // namespace pitlane
