#pragma once

#include "pitlane/order/pms.hpp"

#include <cstdint>

namespace pitlane {

// Pre-trade limits. A zero limit is unset and allows the check. Every check is pure: the caller
// owns counters, cash, and position state. Cash is checked last so a missing cash line never
// hides a harder limit.

struct RmsLimits {
    int64_t max_order_qty{0};
    int64_t max_order_notional{0};
    int64_t max_pos_long{0};
    int64_t max_pos_short{0};
    int64_t max_daily_notional{0};
    int32_t max_orders_per_sec{0};
    int64_t price_band_ppm{0};
    int32_t max_live_orders{0};
    int64_t tick_size{0};  // 0 = unchecked
    int64_t lot_size{0};   // 0 = unchecked
};

enum class RmsCode : uint8_t {
    Ok,
    Qty,
    Notional,
    Position,
    OpenOrders,
    Rate,
    PriceBand,
    Cash,
    Halted,
    Short,
    Tick,
    Lot,
    Biz
};

inline const char* rms_code_name(RmsCode c) noexcept {
    switch (c) {
        case RmsCode::Ok: return "ok";
        case RmsCode::Qty: return "qty";
        case RmsCode::Notional: return "notional";
        case RmsCode::Position: return "position";
        case RmsCode::OpenOrders: return "open orders";
        case RmsCode::Rate: return "rate";
        case RmsCode::PriceBand: return "price band";
        case RmsCode::Cash: return "cash";
        case RmsCode::Halted: return "halted";
        case RmsCode::Short: return "short not allowed";
        case RmsCode::Tick: return "price tick";
        case RmsCode::Lot: return "lot size";
        case RmsCode::Biz: return "business";
    }
    return "rejected";
}

struct RmsReq {
    uint32_t acct{0};
    uint32_t name{0};
    uint8_t side{0};
    uint8_t biz{0};  // Biz; non-directional postings skip the trading limits
    int64_t px{0};
    int64_t qty{0};
    int64_t last_px{0};
    int64_t daily_notional{0};
    int32_t orders_in_window{0};
    int32_t live_orders{0};
    int64_t free_cash{0};
    const PmsPos* pos{nullptr};
    // Products that cannot open a short lot (A-shares, ETFs, bonds) set this
    // false; a sell may then only cover an existing long.
    bool allow_short{true};
};

class Rms {
public:
    void set(const RmsLimits& limits) noexcept { lim_ = limits; }

    const RmsLimits& limits() const noexcept { return lim_; }

    void halt(bool on) noexcept { halted_ = on ? 1 : 0; }

    bool halted() const noexcept { return halted_ != 0; }

    RmsCode check(const RmsReq& r) const noexcept {
        const Biz biz = static_cast<Biz>(r.biz);
        if (!biz_known(biz)) return RmsCode::Biz;
        if (halted_ != 0) return RmsCode::Halted;
        if (r.qty <= 0) return RmsCode::Qty;
        // Collateral transfers, lock/unlock, repayments, and split/merge are
        // local ledger postings, not orders: they must not consume trading
        // limits or require a price.
        if (!biz_directional(biz)) return RmsCode::Ok;
        if (!known_side(r.side) || r.px <= 0) return RmsCode::Qty;
        if (lim_.tick_size > 0 && r.px % lim_.tick_size != 0) return RmsCode::Tick;
        if (lim_.lot_size > 0 && r.qty % lim_.lot_size != 0) return RmsCode::Lot;
        if (lim_.max_order_qty > 0 && r.qty > lim_.max_order_qty) return RmsCode::Qty;
        if (r.px > INT64_MAX / r.qty) return RmsCode::Notional;
        const int64_t notional = r.px * r.qty;
        if (lim_.max_order_notional > 0 && notional > lim_.max_order_notional) return RmsCode::Notional;
        if (lim_.max_daily_notional > 0 &&
            r.daily_notional > lim_.max_daily_notional - notional) {
            return RmsCode::Notional;
        }
        if (r.last_px > 0 && lim_.price_band_ppm > 0) {
            const int64_t diff = r.px >= r.last_px ? r.px - r.last_px : r.last_px - r.px;
            if (diff > INT64_MAX / 1000000) return RmsCode::PriceBand;
            const int64_t lhs = diff * 1000000;
            if (lim_.price_band_ppm > INT64_MAX / r.last_px) return RmsCode::PriceBand;
            const int64_t rhs = r.last_px * lim_.price_band_ppm;
            if (lhs > rhs) return RmsCode::PriceBand;
        }
        if (r.pos != nullptr) {
            if (r.side == static_cast<uint8_t>(Side::Buy) && lim_.max_pos_long > 0 &&
                r.pos->long_qty > lim_.max_pos_long - r.qty) {
                return RmsCode::Position;
            }
            if (r.side == static_cast<uint8_t>(Side::Sell) && lim_.max_pos_short > 0 &&
                r.pos->short_qty > lim_.max_pos_short - r.qty) {
                return RmsCode::Position;
            }
        }
        if (!r.allow_short && r.side == static_cast<uint8_t>(Side::Sell)) {
            const int64_t avail = r.pos ? r.pos->long_qty - r.pos->short_qty : 0;
            if (avail < r.qty) return RmsCode::Short;
        }
        if (lim_.max_live_orders > 0 && r.live_orders >= lim_.max_live_orders) return RmsCode::OpenOrders;
        if (lim_.max_orders_per_sec > 0 && r.orders_in_window >= lim_.max_orders_per_sec) return RmsCode::Rate;
        if (r.free_cash >= 0 && notional > r.free_cash) return RmsCode::Cash;
        return RmsCode::Ok;
    }

private:
    RmsLimits lim_{};
    uint8_t halted_{0};
};

}  // namespace pitlane
