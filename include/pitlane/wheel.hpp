#pragma once

#include <atomic>
#include <cstdint>

namespace pitlane {

// A hierarchical wheel of caller-chosen ticks. One thread calls take.
// Other threads may post. A full wheel refuses the new timer. Tag 0 is rejected.
// The library does not call the timer and does not sleep.

template <int Cap>
class TickWheel {
public:
    static_assert(Cap >= 8 && Cap <= 4096, "timers");

    TickWheel() noexcept {
        for (int i = 0; i < 64; ++i) {
            near_[i] = -1;
            far_[i] = -1;
        }
        for (int i = 0; i < Cap; ++i) {
            node_[i].next = (i + 1 < Cap) ? i + 1 : -1;
            node_[i].pend = -1;
            node_[i].tag = 0;
        }
        free_.store(pack(0, 1), std::memory_order_relaxed);
        pending_.store(0, std::memory_order_relaxed);
    }

    TickWheel(const TickWheel&) = delete;
    TickWheel& operator=(const TickWheel&) = delete;

    bool arm(uint32_t when, uint32_t tag) noexcept {
        if (tag == 0) return false;
        const int idx = alloc();
        if (idx < 0) return false;
        node_[idx].when = when;
        node_[idx].tag = tag;
        link(idx);
        return true;
    }

    bool post(uint32_t when, uint32_t tag) noexcept {
        if (tag == 0) return false;
        const int idx = alloc();
        if (idx < 0) return false;
        node_[idx].when = when;
        node_[idx].tag = tag;
        uint64_t old = pending_.load(std::memory_order_acquire);
        for (;;) {
            node_[idx].pend = index_of(old);
            const uint64_t neu = pack(idx, (old >> 32) + 1ull);
            if (pending_.compare_exchange_weak(old, neu, std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
    }

    bool cancel(uint32_t tag) noexcept {
        if (tag == 0) return false;
        drain();
        for (int b = 0; b < 64; ++b) {
            if (unlink(near_[b], tag)) {
                if (near_[b] < 0) busy_ &= ~(1ull << b);
                return true;
            }
            if (unlink(far_[b], tag)) return true;
        }
        return unlink(later_, tag);
    }

    int take(uint32_t limit, uint32_t* out, int cap) noexcept {
        if (!out || cap <= 0) return 0;
        drain();
        int n = 0;
        if (near_[now_ & kMask] >= 0) {
            n += fire(now_ & kMask, out, cap);
            if (near_[now_ & kMask] >= 0) return n;
        }
        while (earlier(now_, limit)) {
            if (n == cap) return n;
            ++now_;
            if ((now_ & kMask) == 0) {
                cascade();
                pull_later();
            }
            n += fire(now_ & kMask, out + n, cap - n);
            if (near_[now_ & kMask] >= 0) return n;
        }
        return n;
    }

    uint32_t now() const noexcept { return now_; }

    uint32_t next_deadline() const noexcept {
        const uint32_t b = now_ & kMask;
        if (near_[b] >= 0) return now_;
        if (b + 1 < kN) {
            const uint64_t hi = busy_ >> (b + 1);
            if (hi) return (now_ & ~kMask) + (b + 1) + static_cast<uint32_t>(__builtin_ctzll(hi));
        }
        return (now_ & ~kMask) + kN;
    }

private:
    static constexpr uint32_t kN = 64;
    static constexpr uint32_t kMask = kN - 1;
    static constexpr uint32_t kHorizon = kN * kN;

    struct Node {
        int32_t next{-1};
        int32_t pend{-1};
        uint32_t when{0};
        uint32_t tag{0};
    };

    static uint64_t pack(int index, uint64_t version) noexcept {
        return (version << 32) | static_cast<uint32_t>(index + 1);
    }

    static int index_of(uint64_t word) noexcept {
        const uint32_t slot = static_cast<uint32_t>(word);
        return slot == 0 ? -1 : static_cast<int>(slot) - 1;
    }

    static bool earlier(uint32_t a, uint32_t b) noexcept { return static_cast<int32_t>(a - b) < 0; }

    int alloc() noexcept {
        uint64_t old = free_.load(std::memory_order_acquire);
        for (;;) {
            const int idx = index_of(old);
            if (idx < 0) return -1;
            const int nxt = node_[idx].next;
            const uint64_t neu = nxt < 0 ? ((old >> 32) + 1ull) << 32 : pack(nxt, (old >> 32) + 1ull);
            if (free_.compare_exchange_weak(old, neu, std::memory_order_acq_rel, std::memory_order_acquire)) {
                node_[idx].next = -1;
                node_[idx].pend = -1;
                return idx;
            }
        }
    }

    void release(int idx) noexcept {
        node_[idx].tag = 0;
        uint64_t old = free_.load(std::memory_order_relaxed);
        for (;;) {
            node_[idx].next = index_of(old);
            const uint64_t neu = pack(idx, (old >> 32) + 1ull);
            if (free_.compare_exchange_weak(old, neu, std::memory_order_release, std::memory_order_relaxed)) return;
        }
    }

    void insert_near(int idx) noexcept {
        const uint32_t b = node_[idx].when & kMask;
        node_[idx].next = near_[b];
        near_[b] = idx;
        busy_ |= 1ull << b;
    }

    void link(int idx) noexcept {
        if (!earlier(now_, node_[idx].when)) node_[idx].when = now_ + 1u;
        const uint32_t delta = node_[idx].when - now_;
        if (delta < kN) {
            insert_near(idx);
        } else if (delta < kHorizon) {
            const uint32_t b = (node_[idx].when >> 6) & kMask;
            node_[idx].next = far_[b];
            far_[b] = idx;
        } else {
            node_[idx].next = later_;
            later_ = idx;
        }
    }

    bool unlink(int32_t& head, uint32_t tag) noexcept {
        int32_t* p = &head;
        while (*p >= 0) {
            Node& n = node_[*p];
            if (n.tag == tag) {
                const int idx = *p;
                *p = n.next;
                release(idx);
                return true;
            }
            p = &n.next;
        }
        return false;
    }

    void cascade() noexcept {
        const uint32_t b = (now_ >> 6) & kMask;
        int list = far_[b];
        far_[b] = -1;
        while (list >= 0) {
            const int idx = list;
            list = node_[idx].next;
            insert_near(idx);
        }
    }

    void pull_later() noexcept {
        int list = later_;
        later_ = -1;
        while (list >= 0) {
            const int idx = list;
            list = node_[idx].next;
            node_[idx].next = -1;
            link(idx);
        }
    }

    int fire(uint32_t bucket, uint32_t* out, int cap) noexcept {
        int n = 0;
        while (near_[bucket] >= 0 && n < cap) {
            const int idx = near_[bucket];
            near_[bucket] = node_[idx].next;
            if (near_[bucket] < 0) busy_ &= ~(1ull << bucket);
            out[n++] = node_[idx].tag;
            release(idx);
        }
        return n;
    }

    int pop_pending() noexcept {
        uint64_t old = pending_.load(std::memory_order_acquire);
        for (;;) {
            const int idx = index_of(old);
            if (idx < 0) return -1;
            const int nxt = node_[idx].pend;
            const uint64_t neu = nxt < 0 ? ((old >> 32) + 1ull) << 32 : pack(nxt, (old >> 32) + 1ull);
            if (pending_.compare_exchange_weak(old, neu, std::memory_order_acq_rel, std::memory_order_acquire))
                return idx;
        }
    }

    void drain() noexcept {
        for (;;) {
            const int idx = pop_pending();
            if (idx < 0) return;
            link(idx);
        }
    }

    Node node_[Cap]{};
    int32_t near_[64]{};
    int32_t far_[64]{};
    int32_t later_{-1};
    uint64_t busy_{0};
    uint32_t now_{0};
    std::atomic<uint64_t> free_{0};
    std::atomic<uint64_t> pending_{0};
};

}  // namespace pitlane
