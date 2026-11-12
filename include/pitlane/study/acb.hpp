#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

enum class LotPnL : int8_t { Ok = 0, BadQty = 1, Short = 2 };

// Fixed-capacity FIFO lot queue. Prices and qty are integers.

template <int Cap>
class FifoLot {
public:
    static_assert(Cap >= 1 && Cap <= 4096, "cap");

    int lots() const noexcept { return static_cast<int>(qty_); }
    int64_t realized() const noexcept { return rpnl_; }

    LotPnL buy(int64_t px, int64_t qty) noexcept {
        if (unlikely(px <= 0 || qty <= 0)) return LotPnL::BadQty;
        if (unlikely(n_ >= Cap)) return LotPnL::Short;
        slot_[tail_] = Lot{px, qty};
        tail_ = (tail_ + 1) % Cap;
        ++n_;
        qty_ += qty;
        return LotPnL::Ok;
    }

    LotPnL sell(int64_t px, int64_t qty, int64_t* realized = nullptr) noexcept {
        if (unlikely(px <= 0 || qty <= 0)) return LotPnL::BadQty;
        if (unlikely(qty > qty_)) return LotPnL::Short;
        int64_t left = qty;
        int64_t pnl = 0;
        while (left > 0 && n_ > 0) {
            Lot& oldest = slot_[head_];
            if (oldest.qty <= left) {
                pnl += (px - oldest.px) * oldest.qty;
                left -= oldest.qty;
                qty_ -= oldest.qty;
                head_ = (head_ + 1) % Cap;
                --n_;
            } else {
                pnl += (px - oldest.px) * left;
                oldest.qty -= left;
                qty_ -= left;
                left = 0;
            }
        }
        if (unlikely(left > 0)) return LotPnL::Short;
        rpnl_ += pnl;
        if (realized) *realized = pnl;
        return LotPnL::Ok;
    }

private:
    struct Lot {
        int64_t px{0};
        int64_t qty{0};
    };

    Lot slot_[Cap]{};
    int head_{0};
    int tail_{0};
    int n_{0};
    int64_t qty_{0};
    int64_t rpnl_{0};
};

// Average-cost basis. size > 0 buys / covers short; size < 0 sells / opens short.
// suma is total cost (avg * pos). Integer money only.

struct AcbBook {
    int64_t suma{0};
    int64_t pos{0};
    int64_t rpnl{0};
};

PITLANE_INLINE bool acb_apply(AcbBook& a, int64_t px, int64_t size) noexcept {
    if (unlikely(px <= 0 || size == 0)) return false;
    if (a.pos == 0) {
        a.pos = size;
        a.suma = px * size;
        return true;
    }
    if ((a.pos > 0 && size > 0) || (a.pos < 0 && size < 0)) {
        a.suma += px * size;
        a.pos += size;
        return true;
    }
    if (a.pos > 0) {
        // sell (size < 0)
        const int64_t sell = -size;
        const int64_t use = sell < a.pos ? sell : a.pos;
        const int64_t avg = a.suma / a.pos;
        a.rpnl += (px - avg) * use;
        a.pos -= use;
        a.suma = a.pos == 0 ? 0 : avg * a.pos;
        const int64_t rem = sell - use;
        if (rem > 0) {
            a.pos = -rem;
            a.suma = px * (-rem);
        }
        return true;
    }
    // cover short (pos < 0) with buy (size > 0)
    const int64_t short_qty = -a.pos;
    const int64_t use = size < short_qty ? size : short_qty;
    const int64_t avg = a.suma / a.pos;
    a.rpnl += (avg - px) * use;
    a.pos += use;
    a.suma = a.pos == 0 ? 0 : avg * a.pos;
    const int64_t rem = size - use;
    if (rem > 0) {
        a.pos = rem;
        a.suma = px * rem;
    }
    return true;
}

PITLANE_INLINE int64_t acb_upnl(const AcbBook& a, int64_t mark) noexcept {
    if (a.pos == 0) return 0;
    return mark * a.pos - a.suma;
}

}  // namespace pitlane
