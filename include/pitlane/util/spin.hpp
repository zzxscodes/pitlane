#pragma once

#include "pitlane/time/cycle.hpp"
#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstdint>
#include <ctime>

namespace pitlane {

// TTAS spin lock for a critical section shorter than a context switch.
// The hot rings stay lock-free. This is for caller-owned short shared work.
// Occupies its own cache line so waiters do not share a line with nearby data.

PITLANE_INLINE void rest_brief() noexcept {
    timespec ts{0, 1};
    ::nanosleep(&ts, nullptr);
}

class alignas(kCacheLine) SpinLock {
public:
    PITLANE_INLINE bool try_lock() noexcept {
        int expected = 0;
        return locked_.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
    }

    PITLANE_HOT void lock() noexcept {
        int expected = 0;
        if (likely(locked_.compare_exchange_weak(expected, 1, std::memory_order_acquire, std::memory_order_relaxed))) {
            return;
        }
        for (;;) {
            while (locked_.load(std::memory_order_relaxed) != 0) cpu_relax();
            expected = 0;
            if (locked_.compare_exchange_weak(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    // Contended path: brief sleep so waiters drop the lock line before the next CAS.
    PITLANE_HOT void lock_rest() noexcept {
        for (;;) {
            int expected = 0;
            if (locked_.compare_exchange_weak(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }
            rest_brief();
        }
    }

    PITLANE_INLINE void unlock() noexcept { locked_.store(0, std::memory_order_release); }

private:
    std::atomic<int> locked_{0};
};

static_assert(sizeof(SpinLock) == kCacheLine, "spin lock occupies one line");

}  // namespace pitlane
