#pragma once

#include <cstdint>

namespace pitlane {

// Restores a contiguous sequence. A hole is not skipped.
// A duplicate keeps the first token. A sequence more than two windows ahead is refused.
// Token 0 is empty and cannot be stored.

enum class SeqPut : int8_t { In = 0, Dup = 1, Busy = -1, Far = -2, Bad = -3 };

template <uint32_t Cap>
class SeqWin {
public:
    static_assert((Cap & (Cap - 1u)) == 0u, "power of two");
    static_assert(Cap >= 8u && Cap <= 1024u, "window");

    SeqPut push(uint64_t token, uint32_t seq) noexcept {
        if (token == 0) return SeqPut::Bad;
        if (!live_) {
            min_seq_ = seq;
            live_ = true;
        }
        uint32_t off = seq - min_seq_;
        if (off < Cap) return place(off, token);
        if (off < 2u * Cap) {
            const uint32_t need = off + 1u - Cap;
            if (slide(need) < need) return SeqPut::Busy;
            off = seq - min_seq_;
            return place(off, token);
        }
        return SeqPut::Far;
    }

    int pull(uint64_t* out, int cap) noexcept {
        if (!out || cap <= 0) return 0;
        int n = 0;
        while (n < cap && rt_ != rh_) {
            out[n++] = ready_[rt_];
            ready_[rt_] = 0;
            rt_ = (rt_ + 1u) & kMask;
        }
        while (n < cap && order_[head_] != 0) {
            out[n++] = order_[head_];
            order_[head_] = 0;
            head_ = (head_ + 1u) & kMask;
            ++min_seq_;
        }
        return n;
    }

private:
    static constexpr uint32_t kMask = Cap - 1u;

    bool ready_full() const noexcept { return ((rh_ + 1u) & kMask) == rt_; }

    uint32_t slide(uint32_t need) noexcept {
        uint32_t adv = 0;
        while (adv < need) {
            if (order_[head_] == 0) {
                head_ = (head_ + 1u) & kMask;
                ++adv;
                continue;
            }
            if (ready_full()) break;
            ready_[rh_] = order_[head_];
            order_[head_] = 0;
            head_ = (head_ + 1u) & kMask;
            rh_ = (rh_ + 1u) & kMask;
            ++adv;
        }
        min_seq_ += adv;
        return adv;
    }

    SeqPut place(uint32_t off, uint64_t token) noexcept {
        uint64_t& cell = order_[(head_ + off) & kMask];
        if (cell != 0) return SeqPut::Dup;
        cell = token;
        return SeqPut::In;
    }

    uint64_t order_[Cap]{};
    uint64_t ready_[Cap]{};
    uint32_t min_seq_{0};
    uint32_t head_{0};
    uint32_t rh_{0};
    uint32_t rt_{0};
    bool live_{false};
};

}  // namespace pitlane
