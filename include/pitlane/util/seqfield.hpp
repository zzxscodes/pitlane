#pragma once

#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace pitlane {

// Seqlock field for a small, rarely written record. Even sequence is stable.
// Writers bump odd, store the body, then bump even. Readers retry on tear.
// Prefer this over SnapPub when one cache line of state is enough.

template <class T>
class SeqField {
public:
    static_assert(std::is_trivially_copyable<T>::value, "body");

    PITLANE_INLINE void write(const T& in) noexcept {
        const uint32_t s = seq_.load(std::memory_order_relaxed) + 1u;
        seq_.store(s, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        body_ = in;
        seq_.store(s + 1u, std::memory_order_release);
    }

    PITLANE_HOT bool read(T& out) const noexcept {
        const uint32_t s = seq_.load(std::memory_order_acquire);
        if (unlikely(s & 1u)) return false;
        const T tmp = body_;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (unlikely(seq_.load(std::memory_order_relaxed) != s)) return false;
        out = tmp;
        return true;
    }

    PITLANE_INLINE uint32_t seq() const noexcept { return seq_.load(std::memory_order_acquire); }

private:
    alignas(kCacheLine) std::atomic<uint32_t> seq_{0};
    T body_{};
};

}  // namespace pitlane
