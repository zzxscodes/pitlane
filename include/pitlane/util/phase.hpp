#pragma once

#include "pitlane/util/hint.hpp"

#include <atomic>
#include <climits>
#include <cstdint>

namespace pitlane {

// Signed-ticket phase gate. arrive/depart are wait-free. flip_ready is non-blocking:
// returns false until the previous round has drained (no pause loop in the library).

struct PhaseGate {
    std::atomic<int64_t> entered{0};
    std::atomic<int64_t> done_pos{0};
    std::atomic<int64_t> done_neg{INT64_MIN};
    int64_t pending_seen_{0};
    uint8_t flipping_{0};

    PITLANE_INLINE int64_t arrive() noexcept {
        return entered.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    PITLANE_INLINE void depart(int64_t ticket) noexcept {
        std::atomic<int64_t>& side = ticket < 0 ? done_neg : done_pos;
        side.fetch_add(1, std::memory_order_release);
    }

    // Starts a flip if idle. Returns true when the previous round has finished.
    PITLANE_INLINE bool flip_ready(int64_t& seen_out) noexcept {
        if (!flipping_) {
            const int64_t cur = entered.load(std::memory_order_acquire);
            const bool to_pos = cur < 0;
            const int64_t base = to_pos ? 0 : INT64_MIN;
            std::atomic<int64_t>& incoming = to_pos ? done_pos : done_neg;
            incoming.store(base, std::memory_order_relaxed);
            pending_seen_ = entered.exchange(base, std::memory_order_acq_rel);
            flipping_ = to_pos ? 1 : 2;
        }
        std::atomic<int64_t>& previous = flipping_ == 1 ? done_neg : done_pos;
        if (previous.load(std::memory_order_acquire) != pending_seen_) return false;
        seen_out = pending_seen_;
        flipping_ = 0;
        return true;
    }
};

struct PhaseCounter {
    std::atomic<uint64_t> n{0};
};

struct ActiveSum {
    PhaseGate gate{};
    std::atomic<PhaseCounter*> cell{nullptr};
    PhaseCounter* prev_{nullptr};

    PITLANE_INLINE void add(uint64_t v) noexcept {
        PhaseCounter* c = cell.load(std::memory_order_acquire);
        if (unlikely(!c)) return;
        const int64_t ticket = gate.arrive();
        c->n.fetch_add(v, std::memory_order_relaxed);
        gate.depart(ticket);
    }

    // Begin or poll a rotate. Returns the previous cell when the prior round has drained.
    PITLANE_INLINE PhaseCounter* try_rotate(PhaseCounter* next) noexcept {
        if (!gate.flipping_) prev_ = cell.exchange(next, std::memory_order_acq_rel);
        int64_t seen = 0;
        if (!gate.flip_ready(seen)) return nullptr;
        (void)seen;
        return prev_;
    }
};

}  // namespace pitlane
