#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Integer microstructure helpers. The library does not choose a trade.

struct TopSnap {
    int64_t bid_px{0};
    int64_t ask_px{0};
    int64_t bid_sz{0};
    int64_t ask_sz{0};
};

PITLANE_INLINE int64_t ofi_step(const TopSnap& a, const TopSnap& b) noexcept {
    int64_t e = 0;
    if (b.bid_px > a.bid_px) e += b.bid_sz;
    else if (b.bid_px < a.bid_px) e -= a.bid_sz;
    else e += (b.bid_sz - a.bid_sz);
    if (b.ask_px < a.ask_px) e -= b.ask_sz;
    else if (b.ask_px > a.ask_px) e += a.ask_sz;
    else e -= (b.ask_sz - a.ask_sz);
    return e;
}

struct OfiWin {
    int64_t sum{0};
    TopSnap last{};
    uint8_t primed{0};
};

PITLANE_INLINE void ofi_on(OfiWin& w, const TopSnap& cur) noexcept {
    if (!w.primed) {
        w.last = cur;
        w.primed = 1;
        return;
    }
    w.sum += ofi_step(w.last, cur);
    w.last = cur;
}

enum class Agg : uint8_t { Buy = 1, Sell = 2, None = 0 };

PITLANE_INLINE Agg agg_from_seq(uint64_t bid_no, uint64_t ask_no) noexcept {
    if (bid_no == 0 || ask_no == 0) return Agg::None;
    if (bid_no > ask_no) return Agg::Buy;
    if (ask_no > bid_no) return Agg::Sell;
    return Agg::None;
}

struct FootBin {
    int64_t buy{0};
    int64_t sell{0};
};

PITLANE_INLINE void foot_add(FootBin& b, Agg a, int64_t qty) noexcept {
    if (unlikely(qty <= 0)) return;
    if (a == Agg::Buy) b.buy += qty;
    else if (a == Agg::Sell) b.sell += qty;
}

PITLANE_INLINE int64_t foot_delta(const FootBin& b) noexcept { return b.buy - b.sell; }
PITLANE_INLINE int64_t foot_total(const FootBin& b) noexcept { return b.buy + b.sell; }

template <int Cap>
struct SecMove {
    static_assert(Cap >= 2 && Cap <= 8192, "cap");

    struct PxSnap {
        int64_t us{0};
        int64_t px{0};
    };

    void push(int64_t us, int64_t px) noexcept {
        slot_[wr_] = PxSnap{us, px};
        wr_ = (wr_ + 1) % Cap;
        if (n_ < Cap) ++n_;
    }

    // Parts per 10_000 of (last - anchor) / anchor. Anchor is newest snap with us <= now - span.
    bool move_ppm(int& ppm, int64_t now_us, int64_t span_us) const noexcept {
        if (n_ < 2 || span_us < 0) return false;
        const int64_t need = now_us - span_us;
        int i = (wr_ + Cap - 1) % Cap;
        const int64_t last = slot_[i].px;
        for (int seen = 0; seen < n_; ++seen) {
            if (slot_[i].us <= need && slot_[i].px > 0) {
                ppm = static_cast<int>((last - slot_[i].px) * 10000 / slot_[i].px);
                return true;
            }
            i = (i + Cap - 1) % Cap;
        }
        return false;
    }

private:
    PxSnap slot_[Cap]{};
    int n_{0};
    int wr_{0};
};

template <int W>
struct TradeMidGap {
    static_assert(W >= 1 && W <= 1024, "window");

    bool push(int64_t trade_px, int64_t mid_then) noexcept {
        const int64_t x = trade_px - mid_then;
        run_ += x - gap_[i_];
        gap_[i_] = x;
        i_ = (i_ + 1) % W;
        if (filled_ < W) ++filled_;
        return true;
    }

    bool mean(int64_t& out) const noexcept {
        if (filled_ <= 0) return false;
        out = run_ / filled_;
        return true;
    }

    int filled() const noexcept { return filled_; }

private:
    int64_t gap_[W]{};
    int i_{0};
    int filled_{0};
    int64_t run_{0};
};

}  // namespace pitlane
