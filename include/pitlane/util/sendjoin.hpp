#pragma once

#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace pitlane {

// Multi-producer, single-consumer send ring. A full ring rejects the push.
// The low 16 bits of pack_tail are the index; the high bits are a generation so
// a wrap does not match an old compare-exchange. Payload is written after the
// tail claim; ready is stored with release. Only the consumer advances head.

template <class T, uint32_t Cap>
class SendJoinRing {
public:
    static_assert((Cap & (Cap - 1u)) == 0u, "power of two");
    static_assert(Cap >= 2u && Cap <= 65536u, "slots");
    static_assert(std::is_trivially_copyable<T>::value, "slot");

    PITLANE_HOT bool try_push(const T& item) noexcept {
        for (;;) {
            uint32_t old = pack_tail_.load(std::memory_order_acquire);
            const uint32_t ix = old & kIxMask;
            const uint32_t nxt = (ix + 1u) & kMask;
            const uint32_t hd = head_.load(std::memory_order_acquire) & kIxMask;
            if (unlikely(nxt == hd)) return false;
            const uint32_t neu = (((old >> kIxBits) + 1u) << kIxBits) | nxt;
            if (!pack_tail_.compare_exchange_weak(old, neu, std::memory_order_acq_rel, std::memory_order_acquire)) {
                continue;
            }
            cell_[ix & kMask].body = item;
            cell_[ix & kMask].ready.store(1, std::memory_order_release);
            return true;
        }
    }

    PITLANE_HOT bool try_pop(T& out) noexcept {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        const uint32_t t = pack_tail_.load(std::memory_order_acquire) & kIxMask;
        if (unlikely((h & kIxMask) == t)) return false;
        Cell& c = cell_[h & kMask];
        if (unlikely(c.ready.load(std::memory_order_acquire) == 0)) return false;
        out = c.body;
        c.ready.store(0, std::memory_order_release);
        head_.store(h + 1u, std::memory_order_release);
        return true;
    }

private:
    static constexpr uint32_t kMask = Cap - 1u;
    static constexpr uint32_t kIxBits = 16;
    static constexpr uint32_t kIxMask = (1u << kIxBits) - 1u;

    struct alignas(kCacheLine) Cell {
        std::atomic<uint8_t> ready{0};
        T body{};
    };

    alignas(kCacheLine) std::atomic<uint32_t> head_{0};
    alignas(kCacheLine) std::atomic<uint32_t> pack_tail_{0};
    Cell cell_[Cap]{};
};

}  // namespace pitlane
