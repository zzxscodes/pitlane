#pragma once

#include "pitlane/util/hint.hpp"
#include "pitlane/util/simd.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pitlane {

// Single-producer ring. Each reader owns one bit in 0..63.
// need == 0: the first reader to ack claims the slot.
// need != 0: every set bit must ack before the slot may be reused.
// A full ring fails the write. Nothing is overwritten while a required reader is behind.

template <typename T, uint32_t Cap>
class FanRing {
public:
    static_assert((Cap & (Cap - 1u)) == 0u, "Cap must be a power of two");
    static_assert(std::is_trivially_copyable<T>::value, "slot body must be trivially copyable");

    FanRing() = default;

    // Startup-only soft capacity. cap must be a power of two in [2, Cap].
    // Fails when the ring has already published (tail != 0) or a claim is open.
    bool tune(uint32_t cap) noexcept {
        if (cap < 2u || cap > Cap || (cap & (cap - 1u)) != 0u) return false;
        if (tail_.load(std::memory_order_relaxed) != 0 || claimed_ >= 0) return false;
        cap_ = cap;
        mask_ = cap - 1u;
        return true;
    }

    uint32_t capacity() const noexcept { return cap_; }

    // Reserves the next slot for in-place fill. Returns null when the ring is full.
    // Call publish_claimed after writing the body. One claim may be outstanding.
    PITLANE_HOT T* claim(uint64_t need) noexcept {
        if (unlikely(claimed_ >= 0)) return nullptr;
        const uint64_t pos = tail_.load(std::memory_order_relaxed);
        if (unlikely(blocked(pos))) return nullptr;
        Slot& s = slots_[pos & mask_];
        s.state.store(0, std::memory_order_relaxed);
        s.ack.store(0, std::memory_order_relaxed);
        s.need = need;
        s.seq = pos + 1u;
        claimed_ = static_cast<int64_t>(pos);
        return &s.body;
    }

    // Publishes the outstanding claim. Fails when no claim is open.
    PITLANE_HOT bool publish_claimed() noexcept {
        if (PITLANE_UNLIKELY(claimed_ < 0)) return false;
        const uint64_t pos = static_cast<uint64_t>(claimed_);
        Slot& s = slots_[pos & mask_];
        s.state.store(1, std::memory_order_release);
        tail_.store(pos + 1u, std::memory_order_release);
        claimed_ = -1;
        return true;
    }

    PITLANE_HOT PITLANE_INLINE bool publish(const T& item, uint64_t need) noexcept {
        T* slot = claim(need);
        if (unlikely(!slot)) return false;
        copy_bytes(slot, &item, sizeof(T));
        return publish_claimed();
    }

    // Writes n slots, then advances the tail once. Returns 0 when any slot is still busy,
    // and leaves the ring unchanged. n must be from 1 through capacity().
    PITLANE_HOT int publish_batch(const T* items, int n, uint64_t need) noexcept {
        if (unlikely(claimed_ >= 0 || !items || n <= 0 || static_cast<uint32_t>(n) > cap_)) return 0;
        const uint64_t pos = tail_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            if (unlikely(blocked(pos + static_cast<uint64_t>(i)))) return 0;
        }
        for (int i = 0; i < n; ++i) {
            const uint64_t at = pos + static_cast<uint64_t>(i);
            if (i + 1 < n) prefetch_w(&slots_[(at + 1u) & mask_]);
            write_at(at, items[i], need);
        }
        tail_.store(pos + static_cast<uint64_t>(n), std::memory_order_release);
        return n;
    }

    PITLANE_INLINE uint64_t tail() const noexcept { return tail_.load(std::memory_order_acquire); }

    // Returns a pointer to the published body without copying. Valid until the slot is reused.
    PITLANE_HOT const T* view(uint64_t pos, unsigned reader, bool* fresh) const noexcept {
        if (unlikely(reader >= 64u || pos >= tail_.load(std::memory_order_acquire))) return nullptr;
        const Slot& s = slots_[pos & mask_];
        if (unlikely(s.state.load(std::memory_order_acquire) != 1u)) return nullptr;
        if (unlikely(s.seq != pos + 1u)) return nullptr;
        const uint64_t bit = 1ull << reader;
        const uint64_t ack = s.ack.load(std::memory_order_acquire);
        if (fresh) {
            if (s.need == 0) *fresh = ack == 0;
            else *fresh = (s.need & bit) != 0 && (ack & bit) == 0;
        }
        if (unlikely(s.state.load(std::memory_order_acquire) != 1u || s.seq != pos + 1u)) return nullptr;
        return &s.body;
    }

    // Copies the item at absolute position pos. fresh is true when this reader has not acked it.
    PITLANE_HOT bool at(uint64_t pos, unsigned reader, T& out, bool* fresh) const noexcept {
        const T* p = view(pos, reader, fresh);
        if (unlikely(!p)) return false;
        out = *p;
        return true;
    }

    // Copies the newest published item. Returns false when the ring is empty.
    PITLANE_INLINE bool latest(unsigned reader, T& out) const noexcept {
        const uint64_t n = tail();
        if (unlikely(n == 0)) return false;
        bool fresh = false;
        return at(n - 1, reader, out, &fresh);
    }

    PITLANE_HOT bool ack(uint64_t pos, unsigned reader) noexcept {
        if (unlikely(reader >= 64u)) return false;
        Slot& s = slots_[pos & mask_];
        if (unlikely(s.state.load(std::memory_order_acquire) != 1u || s.seq != pos + 1u)) return false;
        const uint64_t bit = 1ull << reader;
        if (s.need == 0) {
            uint64_t zero = 0;
            return s.ack.compare_exchange_strong(zero, bit, std::memory_order_acq_rel, std::memory_order_acquire);
        }
        if (unlikely((s.need & bit) == 0)) return false;
        s.ack.fetch_or(bit, std::memory_order_acq_rel);
        return true;
    }

    // Acks n positions from pos upward. Stops at the first position this reader cannot ack.
    // Positions already acked stay acked. n below 1 acks nothing.
    PITLANE_HOT int ack_span(uint64_t pos, int n, unsigned reader) noexcept {
        if (unlikely(n <= 0 || reader >= 64u)) return 0;
        int done = 0;
        for (int i = 0; i < n; ++i) {
            if (unlikely(pos > UINT64_MAX - static_cast<uint64_t>(i))) break;
            const uint64_t at = pos + static_cast<uint64_t>(i);
            if (i + 1 < n) prefetch_w(&slots_[(at + 1u) & mask_]);
            if (unlikely(!ack(at, reader))) break;
            ++done;
        }
        return done;
    }

private:
    PITLANE_INLINE bool blocked(uint64_t pos) const noexcept {
        if (likely(pos < cap_)) return false;
        const Slot& s = slots_[pos & mask_];
        const uint64_t st = s.state.load(std::memory_order_acquire);
        if (unlikely(st != 1u)) return false;
        const uint64_t ack = s.ack.load(std::memory_order_acquire);
        const bool done = s.need == 0 ? ack != 0 : (ack & s.need) == s.need;
        return !done;
    }

    PITLANE_INLINE void write_at(uint64_t pos, const T& item, uint64_t need) noexcept {
        Slot& s = slots_[pos & mask_];
        s.state.store(0, std::memory_order_relaxed);
        s.ack.store(0, std::memory_order_relaxed);
        s.need = need;
        copy_bytes(&s.body, &item, sizeof(T));
        s.seq = pos + 1u;
        s.state.store(1, std::memory_order_release);
    }

    struct alignas(kCacheLine) Slot {
        std::atomic<uint64_t> state{0};
        std::atomic<uint64_t> ack{0};
        uint64_t need{0};
        uint64_t seq{0};
        T body{};
    };

    Slot slots_[Cap];
    alignas(kCacheLine) std::atomic<uint64_t> tail_{0};
    uint32_t cap_{Cap};
    uint32_t mask_{Cap - 1u};
    int64_t claimed_{-1};
};

}  // namespace pitlane
