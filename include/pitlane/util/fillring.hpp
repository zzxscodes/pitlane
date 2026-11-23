#pragma once

#include "pitlane/util/hint.hpp"
#include "pitlane/util/simd.hpp"

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace pitlane {

// One producer claims a slot, fills it in place, then publishes it.
// One consumer copies it out. A full ring or a second claim fails. Nothing is overwritten.
// Each slot sits on its own cache line, so a write does not share a line with the next slot.
// Each side keeps a local copy of the other side's index and reloads it only when that copy
// says the ring is full or empty.

template <class T, uint32_t Cap>
class FillRing {
public:
    static_assert((Cap & (Cap - 1u)) == 0u, "power of two");
    static_assert(Cap >= 2u && Cap <= 1024u, "slots");
    static_assert(std::is_trivially_copyable<T>::value, "slot");

    PITLANE_HOT PITLANE_INLINE T* claim() noexcept {
        if (unlikely(held_)) return nullptr;
        if (unlikely(tail_ - head_cache_ >= Cap)) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (unlikely(tail_ - head_cache_ >= Cap)) return nullptr;
        }
        held_ = 1;
        Cell& cell = slot_[tail_ & kMask];
        prefetch_w(&cell);
        return &cell.body;
    }

    PITLANE_HOT PITLANE_INLINE bool publish() noexcept {
        if (unlikely(!held_)) return false;
        held_ = 0;
        ++tail_;
        pub_.store(tail_, std::memory_order_release);
        return true;
    }

    PITLANE_INLINE void cancel() noexcept { held_ = 0; }

    PITLANE_HOT PITLANE_INLINE bool take(T& out) noexcept {
        if (unlikely(cons_ >= pub_cache_)) {
            pub_cache_ = pub_.load(std::memory_order_acquire);
            if (unlikely(cons_ >= pub_cache_)) return false;
        }
        copy_bytes(&out, &slot_[cons_ & kMask].body, sizeof(T));
        ++cons_;
        head_.store(cons_, std::memory_order_release);
        return true;
    }

    // Copies up to cap published slots and stores the consumer index once.
    // Returns 0 when the ring is empty, the output is missing, or cap is below 1.
    PITLANE_HOT int take_span(T* out, int cap) noexcept {
        if (unlikely(!out || cap <= 0)) return 0;
        if (unlikely(cons_ >= pub_cache_)) {
            pub_cache_ = pub_.load(std::memory_order_acquire);
            if (unlikely(cons_ >= pub_cache_)) return 0;
        }
        uint64_t avail = pub_cache_ - cons_;
        if (avail > static_cast<uint64_t>(cap)) avail = static_cast<uint64_t>(cap);
        const int n = static_cast<int>(avail);
        for (int i = 0; i < n; ++i) {
            const uint64_t at = (cons_ + static_cast<uint64_t>(i)) & kMask;
            if (i + 1 < n) prefetch_r(&slot_[(at + 1u) & kMask]);
            copy_bytes(&out[i], &slot_[at].body, sizeof(T));
        }
        cons_ += static_cast<uint64_t>(n);
        head_.store(cons_, std::memory_order_release);
        return n;
    }

private:
    static constexpr uint32_t kMask = Cap - 1u;

    struct alignas(kCacheLine) Cell {
        T body{};
    };

    Cell slot_[Cap]{};
    alignas(kCacheLine) uint64_t tail_{0};
    uint64_t head_cache_{0};
    uint8_t held_{0};
    alignas(kCacheLine) uint64_t cons_{0};
    uint64_t pub_cache_{0};
    alignas(kCacheLine) std::atomic<uint64_t> pub_{0};
    alignas(kCacheLine) std::atomic<uint64_t> head_{0};
};

}  // namespace pitlane
