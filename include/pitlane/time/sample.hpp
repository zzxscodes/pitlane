#pragma once

#include <cstdint>

namespace pitlane {

// A fixed list of samples for a test or a measurement.
// The mean divides toward zero. A percentile uses the nearest rank, rounded up.

template <int Cap>
class SampleBox {
public:
    static_assert(Cap >= 2 && Cap <= 256, "samples");

    int count() const noexcept { return n_; }
    int64_t sum() const noexcept { return sum_; }
    int64_t min() const noexcept { return n_ > 0 ? lo_ : 0; }
    int64_t max() const noexcept { return n_ > 0 ? hi_ : 0; }

    bool add(int64_t v) noexcept {
        if (n_ >= Cap) return false;
        if (n_ > 0) {
            if ((v > 0 && sum_ > INT64_MAX - v) || (v < 0 && sum_ < INT64_MIN - v)) return false;
        }
        v_[n_] = v;
        sum_ += v;
        if (n_ == 0 || v < lo_) lo_ = v;
        if (n_ == 0 || v > hi_) hi_ = v;
        ++n_;
        return true;
    }

    bool mean(int64_t& out) const noexcept {
        if (n_ <= 0) return false;
        out = sum_ / n_;
        return true;
    }

    bool pct(int num, int den, int64_t& out) const noexcept {
        if (n_ <= 0 || den <= 0 || num < 0 || num > den) return false;
        int64_t tmp[Cap];
        for (int i = 0; i < n_; ++i) tmp[i] = v_[i];
        for (int i = 1; i < n_; ++i) {
            const int64_t key = tmp[i];
            int j = i - 1;
            while (j >= 0 && tmp[j] > key) {
                tmp[j + 1] = tmp[j];
                --j;
            }
            tmp[j + 1] = key;
        }
        const int rank = static_cast<int>((static_cast<int64_t>(n_) * num + den - 1) / den) - 1;
        const int at = rank < 0 ? 0 : rank;
        out = tmp[at];
        return true;
    }

private:
    int64_t v_[Cap]{};
    int64_t sum_{0};
    int64_t lo_{0};
    int64_t hi_{0};
    int n_{0};
};

}  // namespace pitlane
