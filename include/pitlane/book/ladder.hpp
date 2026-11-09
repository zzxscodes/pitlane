#pragma once

#include "pitlane/desk/types.hpp"

#include <cstdint>

namespace pitlane {

// A fixed price window. Quantity sits at (price - lo) / step.
// Occupied ticks are bits. A summary bit per word lets the best price skip empty words.
// A price off the step, or outside the window, is refused and the window stays.

template <int Cap>
class PriceWindow {
public:
    static_assert(Cap >= 64 && Cap <= 4096 && Cap % 64 == 0, "price window");
    static_assert(Cap / 64 <= 64, "summary word");

    bool reset(int64_t lo, int64_t step) noexcept {
        if (lo <= 0 || step <= 0) return false;
        lo_ = lo;
        step_ = step;
        for (int s = 0; s < 2; ++s) {
            for (int i = 0; i < Cap; ++i) qty_[s][i] = 0;
            for (int w = 0; w < kWords; ++w) bit_[s][w] = 0;
            sum_[s] = 0;
        }
        return true;
    }

    bool set(uint8_t side, int64_t px, int64_t qty) noexcept {
        int at = 0;
        const int s = side_index(side, px, at);
        if (s < 0 || qty < 0) return false;
        qty_[s][at] = qty;
        const uint64_t mask = 1ull << (at & 63);
        uint64_t& word = bit_[s][at >> 6];
        const int word_at = at >> 6;
        if (qty == 0) {
            word &= ~mask;
            if (word == 0) sum_[s] &= ~(1ull << word_at);
        } else {
            word |= mask;
            sum_[s] |= 1ull << word_at;
        }
        return true;
    }

    bool best(uint8_t side, int64_t& px, int64_t& qty) const noexcept {
        if (step_ <= 0 || !known_side(side)) return false;
        const int s = side == 1 ? 0 : 1;
        const uint64_t summary = sum_[s];
        if (summary == 0) return false;
        if (s == 0) {
            const int w = 63 - __builtin_clzll(summary);
            const int b = 63 - __builtin_clzll(bit_[0][w]);
            return read(0, w * 64 + b, px, qty);
        }
        const int w = __builtin_ctzll(summary);
        const int b = __builtin_ctzll(bit_[1][w]);
        return read(1, w * 64 + b, px, qty);
    }

    // Quantity sitting strictly inside the window and strictly better than px.
    // A bid counts higher prices. An ask counts lower prices. The quantity at px stays out.
    // Overflow leaves out unchanged and returns false.
    bool shown_before(uint8_t side, int64_t px, int64_t& out) const noexcept {
        int at = 0;
        const int s = side_index(side, px, at);
        if (s < 0) return false;
        int64_t sum = 0;
        const bool ok = s == 0 ? add_range(0, at + 1, Cap, sum) : add_range(1, 0, at, sum);
        if (!ok) return false;
        out = sum;
        return true;
    }

private:
    static constexpr int kWords = Cap / 64;

    int side_index(uint8_t side, int64_t px, int& at) const noexcept {
        if (step_ <= 0 || !known_side(side) || px < lo_) return -1;
        const int64_t diff = px - lo_;
        if (diff % step_ != 0) return -1;
        const int64_t ticks = diff / step_;
        if (ticks >= Cap) return -1;
        at = static_cast<int>(ticks);
        return side == 1 ? 0 : 1;
    }

    bool read(int s, int at, int64_t& px, int64_t& qty) const noexcept {
        px = lo_ + static_cast<int64_t>(at) * step_;
        qty = qty_[s][at];
        return true;
    }

    bool add_range(int s, int begin, int end, int64_t& sum) const noexcept {
        if (begin >= end) return true;
        const int w_last = (end - 1) >> 6;
        for (int w = begin >> 6; w <= w_last; ++w) {
            uint64_t word = bit_[s][w];
            const int bit0 = w == (begin >> 6) ? (begin & 63) : 0;
            if (bit0 > 0) word &= ~((1ull << bit0) - 1);
            const int bit1 = w == w_last ? ((end - 1) & 63) : 63;
            if (bit1 < 63) word &= (1ull << (bit1 + 1)) - 1;
            while (word != 0) {
                const int b = __builtin_ctzll(word);
                const int64_t q = qty_[s][w * 64 + b];
                if (q > INT64_MAX - sum) return false;
                sum += q;
                word &= word - 1;
            }
        }
        return true;
    }

    int64_t lo_{0};
    int64_t step_{0};
    int64_t qty_[2][Cap]{};
    uint64_t bit_[2][kWords]{};
    uint64_t sum_[2]{};
};

}  // namespace pitlane
