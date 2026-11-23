#pragma once

#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace pitlane {

// Sequence publish ring with no backpressure. A slow reader loses old slots.
// Producers only claim an index, fill the payload, then publish the sequence.
// FanRing and FillRing still fail when full. Use this ring only when overwrite is intended.

template <class T, int Cap>
class SeqPublishRing {
public:
    static_assert(Cap >= 2 && Cap <= 65536 && (Cap & (Cap - 1)) == 0, "seq publish");
    static_assert(std::is_trivially_copyable<T>::value, "payload");

    SeqPublishRing() noexcept {
        for (int i = 0; i < Cap; ++i) seq_[i].store(static_cast<int64_t>(i), std::memory_order_relaxed);
    }

    // Claims the next slot, runs fill(body), then publishes. Always succeeds.
    template <class Fill>
    PITLANE_HOT void publish(Fill&& fill) noexcept {
        (void)publish_claimed([&](T& body, int64_t) { fill(body); });
    }

    // Same, but hands the claim index to the filler. Returns that index.
    template <class Fill>
    PITLANE_HOT int64_t publish_claimed(Fill&& fill) noexcept {
        const int64_t h = claim_.fetch_add(1, std::memory_order_relaxed);
        const int pos = static_cast<int>(h & kMask);
        fill(body_[pos], h);
        seq_[pos].store(h + Cap, std::memory_order_release);
        return h;
    }

    // Drains from cursor while slots are published. Returns the advanced cursor.
    // `cursor` is the next claim index to read; overwritten slots are skipped.
    template <class OnMsg>
    PITLANE_HOT int64_t drain_from(int64_t cursor, OnMsg&& on_msg) const noexcept {
        for (;;) {
            const int pos = static_cast<int>(cursor & kMask);
            const int64_t published = seq_[pos].load(std::memory_order_acquire);
            if (published < cursor + Cap) break;
            on_msg(body_[pos]);
            ++cursor;
        }
        return cursor;
    }

    int64_t claimed() const noexcept { return claim_.load(std::memory_order_relaxed); }

private:
    static constexpr int64_t kMask = Cap - 1;

    alignas(kCacheLine) T body_[Cap]{};
    alignas(kCacheLine) std::atomic<int64_t> seq_[Cap]{};
    alignas(kCacheLine) std::atomic<int64_t> claim_{0};
};

}  // namespace pitlane
