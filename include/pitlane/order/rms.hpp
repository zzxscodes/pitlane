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
};

enum class RmsCode : uint8_t { Ok, Qty, Notional, Position, OpenOrders, Rate, PriceBand, Cash, Halted };

struct RmsReq {
    uint32_t acct{0};
    uint32_t name{0};
    uint8_t side{0};
    int64_t px{0};
    int64_t qty{0};
    int64_t last_px{0};
    int64_t daily_notional{0};
    int32_t orders_in_window{0};
    int32_t live_orders{0};
    int64_t free_cash{0};
    const PmsPos* pos{nullptr};
};

class Rms {
public:
    void set(const RmsLimits& limits) noexcept { lim_ = limits; }

    const RmsLimits& limits() const noexcept { return lim_; }

    void halt(bool on) noexcept { halted_ = on ? 1 : 0; }

    bool halted() const noexcept { return halted_ != 0; }

    RmsCode check(const RmsReq& r) const noexcept {
        if (halted_ != 0) return RmsCode::Halted;
        if (!known_side(r.side) || r.qty <= 0 || r.px <= 0) return RmsCode::Qty;
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
