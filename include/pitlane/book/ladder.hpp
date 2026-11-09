#pragma once

#include "pitlane/desk/types.hpp"

#include <cstdint>

namespace pitlane {

// A fixed price window. Quantity sits at (price - lo) / step.
// Occupied ticks are bits, so the best price skips empty ticks a word at a time.
// A price off the step, or outside the window, is refused and the window stays.

template <int Cap>
class PriceWindow {
public:
    static_assert(Cap >= 64 && Cap <= 4096 && Cap % 64 == 0, "price window");

    bool reset(int64_t lo, int64_t step) noexcept {
        if (lo <= 0 || step <= 0) return false;
        lo_ = lo;
        step_ = step;
        for (int s = 0; s < 2; ++s) {
            for (int i = 0; i < Cap; ++i) qty_[s][i] = 0;
            for (int w = 0; w < kWords; ++w) bit_[s][w] = 0;
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
        if (qty == 0) word &= ~mask;
        else word |= mask;
        return true;
    }

    bool best(uint8_t side, int64_t& px, int64_t& qty) const noexcept {
        if (step_ <= 0 || !known_side(side)) return false;
        const int s = side == 1 ? 0 : 1;
        if (s == 0) {
            for (int w = kWords - 1; w >= 0; --w) {
                const uint64_t bits = bit_[0][w];
                if (bits == 0) continue;
                const int b = 63 - __builtin_clzll(bits);
                return read(0, w * 64 + b, px, qty);
            }
        } else {
            for (int w = 0; w < kWords; ++w) {
                const uint64_t bits = bit_[1][w];
                if (bits == 0) continue;
                const int b = __builtin_ctzll(bits);
                return read(1, w * 64 + b, px, qty);
            }
        }
        return false;
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

    int64_t lo_{0};
    int64_t step_{0};
    int64_t qty_[2][Cap]{};
    uint64_t bit_[2][kWords]{};
};

}  // namespace pitlane
