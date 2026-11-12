#pragma once

#include "pitlane/desk/codec.hpp"

#include <cstdint>

namespace pitlane {

struct LotPos {
    int64_t total{0};
    int64_t today{0};
};

struct LotPend {
    int64_t close_today{0};
    int64_t close_yday{0};
};

struct LotSlice {
    Off off{Off::Open};
    int64_t qty{0};
};

enum class SplitMode : uint8_t { TodayThenYdayThenOpen = 1, CloseOnly = 2, YdayThenStop = 3 };

// Returns the number of slices written, or -1 for an unsupported mode.
// A slice smaller than min_lot is dropped. max_lot <= 0 means no cap.
int split_lots(int64_t want, LotPos pos, LotPend pend, int64_t max_lot, int64_t min_lot, SplitMode mode, LotSlice* out,
               int cap) noexcept;

// A-share board lot. Direction still goes through normalize_gate; these are quantity helpers only.

inline bool board_lot_ok(int64_t qty, int64_t lot = 100) noexcept {
    return lot > 0 && qty > 0 && qty % lot == 0;
}

inline int64_t board_lot_floor(int64_t qty, int64_t lot = 100) noexcept {
    if (lot <= 0 || qty <= 0) return 0;
    return (qty / lot) * lot;
}

// Margin trading integers. Host owns balances; library only checks room.

struct MarginBook {
    int64_t buying_power{0};  // cash + available credit for buys
    int64_t collateral{0};    // pledged market value
    int64_t debt{0};          // outstanding financing
    int64_t short_avail{0};   // shares available to sell short
};

inline int64_t margin_buy_room(const MarginBook& m, int64_t px, int64_t mult = 1) noexcept {
    if (px <= 0 || mult <= 0 || m.buying_power <= 0) return 0;
    const int64_t step = px * mult;
    if (step <= 0) return 0;
    return m.buying_power / step;
}

inline bool margin_buy_ok(const MarginBook& m, int64_t px, int64_t qty, int64_t mult = 1) noexcept {
    if (qty <= 0) return false;
    return margin_buy_room(m, px, mult) >= qty;
}

inline bool margin_short_ok(const MarginBook& m, int64_t qty) noexcept {
    return qty > 0 && m.short_avail >= qty;
}

inline int64_t margin_collateral_gap(const MarginBook& m, int64_t need_ratio_num, int64_t need_ratio_den) noexcept {
    if (need_ratio_den <= 0 || need_ratio_num < 0 || m.debt <= 0) return 0;
    // required collateral ≈ debt * num / den
    const int64_t need = m.debt * need_ratio_num / need_ratio_den;
    if (m.collateral >= need) return 0;
    return need - m.collateral;
}

}  // namespace pitlane
