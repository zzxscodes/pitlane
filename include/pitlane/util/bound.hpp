#pragma once

#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstdint>

namespace pitlane {

// One unsigned compare covers both a negative index and an upper bound.
PITLANE_INLINE bool index_ok(int i, int n) noexcept {
    return static_cast<unsigned>(i) < static_cast<unsigned>(n);
}

// One unsigned compare for a closed integer range.
PITLANE_INLINE bool range_ok(int v, int lo, int hi) noexcept {
    return static_cast<unsigned>(v) - static_cast<unsigned>(lo) <=
           static_cast<unsigned>(hi) - static_cast<unsigned>(lo);
}

// Power-of-two wrap. Cap must be a power of two.
PITLANE_INLINE int wrap_pow2(int i, int cap) noexcept { return i & (cap - 1); }

// Walk budget from the notes: charge words while following pointers. Soft under-count is fine.
struct WalkBudget {
    alignas(8) std::atomic<uint64_t> left{0};

    explicit WalkBudget(uint64_t words) noexcept : left(words) {}

    PITLANE_INLINE bool charge(uint64_t words) noexcept {
        uint64_t cur = left.load(std::memory_order_relaxed);
        if (words > cur) return false;
        left.store(cur - words, std::memory_order_relaxed);
        return true;
    }
};

struct Walk {
    WalkBudget* budget{nullptr};
    uint32_t depth{64};

    PITLANE_INLINE bool enter(uint64_t words) noexcept {
        if (unlikely(depth == 0 || !budget || !budget->charge(words))) return false;
        --depth;
        return true;
    }

    PITLANE_INLINE void leave() noexcept { ++depth; }
};

}  // namespace pitlane
