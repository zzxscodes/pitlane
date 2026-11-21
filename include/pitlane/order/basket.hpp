#pragma once

#include "pitlane/desk/types.hpp"
#include "pitlane/order/quoteprice.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Per-name target ledger for a basket. Control headers stay in basket_make.

struct BasketLeg {
    uint32_t name{0};
    int64_t target{0};
    int64_t done{0};
    int64_t working{0};
};

// Per-leg execution controls. Zero means off except `granularity`, which is
// the whole-share step applied to each child. `price_type` follows the quote
// mode: 0 crosses the touch, 1 joins it.
struct BasketLegCtl {
    int64_t clip{0};          // largest child, 0 = the whole gap
    int64_t granularity{1};   // round a child down to this step
    int64_t tolerance{0};     // leave the leg alone inside this band
    int64_t begin_us{0};      // 0 = immediately
    int64_t end_us{0};        // 0 = no deadline
    int64_t late_us{0};       // trade past `end_us` for this long, then stop
    int64_t reorder_us{200000};
    int32_t adjust_ticks{0};  // price offset from `price_type`
    uint8_t price_type{0};    // 0 opposite, 1 own side
    uint8_t one_touch{0};     // at a locked market send once, then wait
    uint8_t priority{0};      // lower wins; ties keep insertion order
    uint8_t pad{0};
};

struct BasketChild {
    uint8_t ok{0};
    uint8_t side{0};
    uint8_t style{0};
    uint8_t reason{0};  // 0 normal, 1 locked
    int64_t qty{0};
    int64_t px{0};
    int64_t gap{0};
};

PITLANE_INLINE int64_t basket_gap(const BasketLeg& leg) noexcept {
    const int64_t used = leg.done + leg.working;
    if (leg.target <= used) return 0;
    return leg.target - used;
}

// Round the remaining gap down to a whole lot. lot <= 0 fails closed (0).
PITLANE_INLINE int64_t basket_topup(const BasketLeg& leg, int64_t lot) noexcept {
    if (lot <= 0) return 0;
    const int64_t gap = basket_gap(leg);
    return (gap / lot) * lot;
}

// Next child for one leg against the signed net position. The target is an
// absolute position, so `net` is the account's signed holding and `working`
// is quantity already sent. A gap inside `tolerance` is left alone; a gap
// larger than `clip` is worked in clips. A locked market with `one_touch`
// yields one child and then nothing until `touched` is cleared.
PITLANE_INLINE bool basket_child(const BasketLeg& leg, const BasketLegCtl& ctl, int64_t net, const QuoteTop& q,
                                 int64_t tick, int64_t now_us, BasketChild& out,
                                 uint8_t* touched = nullptr) noexcept {
    out = BasketChild{};
    if (leg.name == 0 || tick <= 0 || ctl.granularity <= 0) return false;
    if (ctl.begin_us > 0 && now_us < ctl.begin_us) return false;
    if (ctl.end_us > 0 && now_us > ctl.end_us + ctl.late_us) return false;
    const bool late = ctl.end_us > 0 && now_us > ctl.end_us;
    if (leg.working < 0) return false;
    if (leg.target > INT64_MAX - leg.working) return false;
    const int64_t used = net + leg.working;
    int64_t gap = leg.target - used;
    if (gap <= ctl.tolerance && gap >= -ctl.tolerance) return false;
    const bool buy = gap > 0;
    int64_t qty = buy ? gap : -gap;
    if (ctl.clip > 0 && qty > ctl.clip) qty = ctl.clip;
    if (qty > ctl.granularity) qty = (qty / ctl.granularity) * ctl.granularity;
    if (qty <= 0) return false;

    const bool locked = q.bid[0] > 0 && q.bid[0] == q.ask[0];
    int64_t px = 0;
    uint8_t style = static_cast<uint8_t>(Style::Day);
    if (locked && ctl.one_touch) {
        if (touched && *touched) return false;
        px = buy ? q.ask[0] : q.bid[0];
        style = static_cast<uint8_t>(Style::Immediate);
        if (touched) *touched = 1;
        out.reason = 1;
    } else {
        px = chasing_px(buy, late ? 0 : ctl.price_type, q, tick, ctl.adjust_ticks);
        if (px <= 0) px = pre_px(buy, late ? 0 : ctl.price_type, q, true, false);
        if (px <= 0) return false;
        if (late || ctl.price_type == 0) style = static_cast<uint8_t>(Style::Immediate);
    }
    out.ok = 1;
    out.side = buy ? static_cast<uint8_t>(Side::Buy) : static_cast<uint8_t>(Side::Sell);
    out.style = style;
    out.qty = qty;
    out.px = px;
    out.gap = gap;
    return true;
}

template <int Cap>
class BasketBook {
public:
    static_assert(Cap >= 1 && Cap <= 2048, "cap");

    uint64_t id() const noexcept { return id_; }
    int legs() const noexcept { return n_; }

    bool open(uint64_t id) noexcept {
        if (id == 0) return false;
        id_ = id;
        n_ = 0;
        for (int i = 0; i < Cap; ++i) leg_[i] = BasketLeg{};
        for (int i = 0; i < Cap; ++i) ctl_[i] = BasketLegCtl{};
        return true;
    }

    int find(uint32_t name) const noexcept {
        if (name == 0) return -1;
        for (int i = 0; i < n_; ++i) {
            if (leg_[i].name == name) return i;
        }
        return -1;
    }

    bool put(uint32_t name, int64_t target) noexcept {
        if (id_ == 0 || name == 0 || target < 0) return false;
        const int i = find(name);
        if (i >= 0) {
            leg_[i].target = target;
            return true;
        }
        if (n_ >= Cap) return false;
        leg_[n_] = BasketLeg{};
        leg_[n_].name = name;
        leg_[n_].target = target;
        ++n_;
        return true;
    }

    BasketLeg* at(int i) noexcept {
        if (i < 0 || i >= n_) return nullptr;
        return &leg_[i];
    }

    bool set_ctl(uint32_t name, const BasketLegCtl& ctl) noexcept {
        const int i = find(name);
        if (i < 0) return false;
        if (ctl.granularity <= 0) return false;
        ctl_[i] = ctl;
        return true;
    }

    const BasketLegCtl* ctl_at(int i) const noexcept {
        if (i < 0 || i >= n_) return nullptr;
        return &ctl_[i];
    }

    const BasketLeg* at(int i) const noexcept {
        if (i < 0 || i >= n_) return nullptr;
        return &leg_[i];
    }

    bool add_done(uint32_t name, int64_t qty) noexcept {
        BasketLeg* leg = mut(name);
        if (!leg || qty <= 0) return false;
        if (leg->working < qty) return false;
        leg->working -= qty;
        leg->done += qty;
        return true;
    }

    bool add_working(uint32_t name, int64_t qty) noexcept {
        BasketLeg* leg = mut(name);
        if (!leg || qty <= 0) return false;
        if (basket_gap(*leg) < qty) return false;
        leg->working += qty;
        return true;
    }

    bool drop_working(uint32_t name, int64_t qty) noexcept {
        BasketLeg* leg = mut(name);
        if (!leg || qty <= 0 || leg->working < qty) return false;
        leg->working -= qty;
        return true;
    }

private:
    BasketLeg* mut(uint32_t name) noexcept {
        const int i = find(name);
        return i < 0 ? nullptr : &leg_[i];
    }

    uint64_t id_{0};
    int n_{0};
    BasketLeg leg_[Cap]{};
    BasketLegCtl ctl_[Cap]{};
};

}  // namespace pitlane
