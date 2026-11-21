#pragma once

#include <cstdint>

namespace pitlane {

// The high and low of the last Cap prices the caller pushed.
// Cap is a power of two from 4 to 256. A non-positive price is refused and the window stays.
// Each price enters each side once and leaves once. The front of that side is the extreme.
// The window does not choose a trade.

template <int Cap>
class ExtremeWindow {
public:
    static_assert(Cap >= 4 && Cap <= 256 && (Cap & (Cap - 1)) == 0, "extreme window");

    bool push(int64_t px) noexcept {
        if (px <= 0) return false;
        if (n_ == Cap) drop(seq_ - static_cast<uint64_t>(Cap));
        px_[seq_ & kMask] = px;
        while (hi_n_ != 0) {
            const uint64_t back = hi_at(hi_n_ - 1);
            if (px_[back & kMask] > px) break;
            --hi_n_;
        }
        hi_[(hi_h_ + hi_n_) & kMask] = seq_;
        ++hi_n_;
        while (lo_n_ != 0) {
            const uint64_t back = lo_at(lo_n_ - 1);
            if (px_[back & kMask] < px) break;
            --lo_n_;
        }
        lo_[(lo_h_ + lo_n_) & kMask] = seq_;
        ++lo_n_;
        ++seq_;
        if (n_ < Cap) ++n_;
        return true;
    }

    bool high(int64_t& px) const noexcept {
        if (hi_n_ == 0) return false;
        px = px_[hi_[hi_h_] & kMask];
        return true;
    }

    bool low(int64_t& px) const noexcept {
        if (lo_n_ == 0) return false;
        px = px_[lo_[lo_h_] & kMask];
        return true;
    }

    int size() const noexcept { return n_; }

    void clear() noexcept {
        seq_ = 0;
        n_ = 0;
        hi_n_ = 0;
        lo_n_ = 0;
        hi_h_ = 0;
        lo_h_ = 0;
    }

private:
    static constexpr uint64_t kMask = static_cast<uint64_t>(Cap - 1);

    uint64_t hi_at(int n) const noexcept { return hi_[(hi_h_ + n) & kMask]; }
    uint64_t lo_at(int n) const noexcept { return lo_[(lo_h_ + n) & kMask]; }

    void drop(uint64_t old) noexcept {
        if (hi_n_ != 0 && hi_[hi_h_] == old) {
            hi_h_ = (hi_h_ + 1) & kMask;
            --hi_n_;
        }
        if (lo_n_ != 0 && lo_[lo_h_] == old) {
            lo_h_ = (lo_h_ + 1) & kMask;
            --lo_n_;
        }
    }

    int64_t px_[Cap]{};
    uint64_t hi_[Cap]{};
    uint64_t lo_[Cap]{};
    uint64_t seq_{0};
    uint64_t hi_h_{0};
    uint64_t lo_h_{0};
    int n_{0};
    int hi_n_{0};
    int lo_n_{0};
};

}  // namespace pitlane
