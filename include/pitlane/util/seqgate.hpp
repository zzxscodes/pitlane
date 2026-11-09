#pragma once

#include <cstdint>

namespace pitlane {

// Live numbers wait in a fixed side list while a gap is filled in order.
// The library reports the missing range and does not ask the counter to resend.

template <int Cap>
class SeqGate {
public:
    static_assert(Cap >= 2 && Cap <= 64, "side");

    bool start(uint64_t next) noexcept {
        if (next == 0) return false;
        last_ = next - 1;
        side_n_ = 0;
        recover_ = 0;
        miss_lo_ = 0;
        miss_hi_ = 0;
        started_ = 1;
        return true;
    }

    bool recovering() const noexcept { return recover_ != 0; }
    int parked() const noexcept { return side_n_; }
    uint64_t last() const noexcept { return last_; }

    bool missing(uint64_t& lo, uint64_t& hi) const noexcept {
        if (!recover_) return false;
        lo = miss_lo_;
        hi = miss_hi_;
        return true;
    }

    // 1 when this number is released. 0 when it is stale, inside the gap, or parked.
    // -2 when the side list is full and nothing changed. -3 for a bad call. -4 when the output cannot take a release.
    int live(uint64_t seq, uint64_t* out, int cap) noexcept {
        if (!started_ || seq == 0) return -3;
        if (seq <= last_) return 0;
        if (recover_) {
            if (seq <= miss_hi_) return 0;
            if (!park(seq)) return -2;
            return 0;
        }
        if (seq == last_ + 1) {
            if (!out || cap < 1) return -4;
            out[0] = seq;
            last_ = seq;
            return 1;
        }
        if (!park(seq)) return -2;
        miss_lo_ = last_ + 1;
        miss_hi_ = seq - 1;
        recover_ = 1;
        return 0;
    }

    // Writes the replayed number and any side numbers that now follow it.
    // -1 when the number is not the next missing one. -4 when the output is short. State stays put on failure.
    int replay(uint64_t seq, uint64_t* out, int cap) noexcept {
        if (!started_ || !recover_ || seq == 0 || !out) return -3;
        if (seq != last_ + 1) return -1;
        uint64_t tmp[Cap];
        int tn = side_n_;
        for (int i = 0; i < tn; ++i) tmp[i] = side_[i];
        uint64_t emit[Cap + 1];
        int en = 0;
        emit[en++] = seq;
        uint64_t last = seq;
        uint64_t mlo = miss_lo_;
        uint64_t mhi = miss_hi_;
        uint8_t rec = 1;
        if (last == mhi) {
            rec = 0;
            for (;;) {
                int at = -1;
                for (int i = 0; i < tn; ++i) {
                    if (tmp[i] == last + 1) {
                        at = i;
                        break;
                    }
                }
                if (at < 0) break;
                if (en >= Cap + 1) break;
                emit[en++] = last + 1;
                last = last + 1;
                tmp[at] = tmp[tn - 1];
                --tn;
            }
            uint64_t mn = UINT64_MAX;
            int w = 0;
            for (int i = 0; i < tn; ++i) {
                if (tmp[i] <= last) continue;
                if (tmp[i] < mn) mn = tmp[i];
                tmp[w++] = tmp[i];
            }
            tn = w;
            if (tn > 0) {
                rec = 1;
                mlo = last + 1;
                mhi = mn - 1;
            }
        }
        if (en > cap) return -4;
        for (int i = 0; i < en; ++i) out[i] = emit[i];
        for (int i = 0; i < tn; ++i) side_[i] = tmp[i];
        side_n_ = tn;
        last_ = last;
        miss_lo_ = mlo;
        miss_hi_ = mhi;
        recover_ = rec;
        return en;
    }

private:
    bool park(uint64_t seq) noexcept {
        for (int i = 0; i < side_n_; ++i) {
            if (side_[i] == seq) return true;
        }
        if (side_n_ >= Cap) return false;
        side_[side_n_++] = seq;
        return true;
    }

    uint64_t side_[Cap]{};
    uint64_t last_{0};
    uint64_t miss_lo_{0};
    uint64_t miss_hi_{0};
    int side_n_{0};
    uint8_t recover_{0};
    uint8_t started_{0};
};

}  // namespace pitlane
