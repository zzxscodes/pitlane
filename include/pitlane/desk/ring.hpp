#pragma once

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

    bool publish(const T& item, uint64_t need) noexcept {
        const uint64_t pos = tail_.load(std::memory_order_relaxed);
        if (blocked(pos)) return false;
        write_at(pos, item, need);
        tail_.store(pos + 1u, std::memory_order_release);
        return true;
    }

    // Writes n slots, then advances the tail once. Returns 0 when any slot is still busy,
    // and leaves the ring unchanged. n must be from 1 through Cap.
    int publish_batch(const T* items, int n, uint64_t need) noexcept {
        if (!items || n <= 0 || static_cast<uint32_t>(n) > Cap) return 0;
        const uint64_t pos = tail_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            if (blocked(pos + static_cast<uint64_t>(i))) return 0;
        }
        for (int i = 0; i < n; ++i) write_at(pos + static_cast<uint64_t>(i), items[i], need);
        tail_.store(pos + static_cast<uint64_t>(n), std::memory_order_release);
        return n;
    }

    uint64_t tail() const noexcept { return tail_.load(std::memory_order_acquire); }

    // Copies the item at absolute position pos. fresh is true when this reader has not acked it.
    bool at(uint64_t pos, unsigned reader, T& out, bool* fresh) const noexcept {
        if (reader >= 64u || pos >= tail_.load(std::memory_order_acquire)) return false;
        const Slot& s = slots_[pos & (Cap - 1u)];
        if (s.state.load(std::memory_order_acquire) != 1u) return false;
        if (s.seq != pos + 1u) return false;
        T tmp = s.body;
        if (s.state.load(std::memory_order_acquire) != 1u || s.seq != pos + 1u) return false;
        out = tmp;
        const uint64_t bit = 1ull << reader;
        const uint64_t ack = s.ack.load(std::memory_order_acquire);
        if (fresh) {
            if (s.need == 0) *fresh = ack == 0;
            else *fresh = (s.need & bit) != 0 && (ack & bit) == 0;
        }
        return true;
    }

    // Copies the newest published item. Returns false when the ring is empty.
    bool latest(unsigned reader, T& out) const noexcept {
        const uint64_t n = tail();
        if (n == 0) return false;
        bool fresh = false;
        return at(n - 1, reader, out, &fresh);
    }

    bool ack(uint64_t pos, unsigned reader) noexcept {
        if (reader >= 64u) return false;
        Slot& s = slots_[pos & (Cap - 1u)];
        if (s.state.load(std::memory_order_acquire) != 1u || s.seq != pos + 1u) return false;
        const uint64_t bit = 1ull << reader;
        if (s.need == 0) {
            uint64_t zero = 0;
            return s.ack.compare_exchange_strong(zero, bit, std::memory_order_acq_rel, std::memory_order_acquire);
        }
        if ((s.need & bit) == 0) return false;
        s.ack.fetch_or(bit, std::memory_order_acq_rel);
        return true;
    }

    // Acks n positions from pos upward. Stops at the first position this reader cannot ack.
    // Positions already acked stay acked. n below 1 acks nothing.
    int ack_span(uint64_t pos, int n, unsigned reader) noexcept {
        if (n <= 0 || reader >= 64u) return 0;
        int done = 0;
        for (int i = 0; i < n; ++i) {
            if (pos > UINT64_MAX - static_cast<uint64_t>(i)) break;
            if (!ack(pos + static_cast<uint64_t>(i), reader)) break;
            ++done;
        }
        return done;
    }

private:
    bool blocked(uint64_t pos) const noexcept {
        if (pos < Cap) return false;
        const Slot& s = slots_[pos & (Cap - 1u)];
        const uint64_t st = s.state.load(std::memory_order_acquire);
        if (st != 1u) return false;
        const uint64_t ack = s.ack.load(std::memory_order_acquire);
        const bool done = s.need == 0 ? ack != 0 : (ack & s.need) == s.need;
        return !done;
    }

    void write_at(uint64_t pos, const T& item, uint64_t need) noexcept {
        Slot& s = slots_[pos & (Cap - 1u)];
        s.state.store(0, std::memory_order_relaxed);
        s.ack.store(0, std::memory_order_relaxed);
        s.need = need;
        s.body = item;
        s.seq = pos + 1u;
        s.state.store(1, std::memory_order_release);
    }

    struct alignas(64) Slot {
        std::atomic<uint64_t> state{0};
        std::atomic<uint64_t> ack{0};
        uint64_t need{0};
        uint64_t seq{0};
        T body{};
    };

    Slot slots_[Cap];
    alignas(64) std::atomic<uint64_t> tail_{0};
};

}  // namespace pitlane
