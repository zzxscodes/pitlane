#pragma once

#include "pitlane/types.hpp"

#include <cstdint>

namespace pitlane {

// A clock the caller steps, a budget that starts failing, and checked records for tests.
// The library does not sleep and does not run a session.

struct StepClock {
    int64_t now{0};

    bool jump(int64_t t) noexcept {
        now = t;
        return true;
    }

    bool step(int64_t dt) noexcept {
        if (dt < 0 || now > INT64_MAX - dt) return false;
        now += dt;
        return true;
    }
};

struct FailBudget {
    int left{0};

    void arm(int n) noexcept { left = n > 0 ? n : 0; }

    bool allow() noexcept {
        if (left <= 0) return false;
        --left;
        return true;
    }
};

template <int Cap>
class StepList {
public:
    static_assert(Cap >= 2 && Cap <= 256, "steps");

    int size() const noexcept { return n_; }

    bool push(int64_t at, uint64_t tag) noexcept {
        if (tag == 0 || n_ >= Cap) return false;
        if (n_ > 0 && at < row_[n_ - 1].at) return false;
        row_[n_].at = at;
        row_[n_].tag = tag;
        ++n_;
        return true;
    }

    int take(int64_t now, uint64_t* tags, int cap) noexcept {
        if (!tags || cap <= 0) return -1;
        int due = 0;
        while (due < n_ && row_[due].at <= now) ++due;
        if (due == 0) return 0;
        const int give = due < cap ? due : cap;
        for (int i = 0; i < give; ++i) tags[i] = row_[i].tag;
        const int rest = n_ - give;
        for (int i = 0; i < rest; ++i) row_[i] = row_[give + i];
        n_ = rest;
        return give;
    }

private:
    struct Row {
        int64_t at{0};
        uint64_t tag{0};
    };

    Row row_[Cap]{};
    int n_{0};
};

enum class RowFault : uint8_t { None = 0, Arg = 1, Px = 2, Qty = 3, Order = 4 };

inline RowFault ladder_check(uint8_t side, const int64_t* px, const int64_t* qty, int n) noexcept {
    if (n < 0) return RowFault::Arg;
    if (n == 0) return RowFault::None;
    if (!px || !qty || !known_side(side)) return RowFault::Arg;
    const bool buy = buy_side(side);
    for (int i = 0; i < n; ++i) {
        if (px[i] <= 0) return RowFault::Px;
        if (qty[i] <= 0) return RowFault::Qty;
        if (i == 0) continue;
        if (buy && px[i] >= px[i - 1]) return RowFault::Order;
        if (!buy && px[i] <= px[i - 1]) return RowFault::Order;
    }
    return RowFault::None;
}

inline bool known_act(uint8_t act) noexcept {
    return act == static_cast<uint8_t>(Act::Enter) || act == static_cast<uint8_t>(Act::Pull);
}

inline bool known_style(uint8_t style) noexcept {
    return style == static_cast<uint8_t>(Style::Day) || style == static_cast<uint8_t>(Style::Passive) ||
           style == static_cast<uint8_t>(Style::Immediate) || style == static_cast<uint8_t>(Style::All);
}

inline bool known_phase(uint8_t phase) noexcept { return phase <= static_cast<uint8_t>(Phase::Expired); }

inline bool intent_make(Intent& out, uint64_t tag, uint32_t name, uint32_t acct, int64_t px, int64_t qty, int64_t stamp_us,
                        uint8_t side, uint8_t act, uint8_t style, int32_t lot) noexcept {
    out = Intent{};
    if (tag == 0 || name == 0 || px <= 0 || qty <= 0 || stamp_us < 0 || lot <= 0) return false;
    if (!known_side(side) || !known_act(act) || !known_style(style)) return false;
    out.tag = tag;
    out.name = name;
    out.acct = acct;
    out.px = px;
    out.qty = qty;
    out.stamp_us = stamp_us;
    out.side = side;
    out.act = act;
    out.style = style;
    out.lot = lot;
    return true;
}

inline bool report_make(Report& out, uint64_t tag, uint64_t contra, uint32_t name, int64_t px, int64_t qty, int64_t left,
                        int64_t stamp_us, uint16_t why, uint8_t phase, uint8_t side) noexcept {
    out = Report{};
    if (tag == 0 || px < 0 || qty < 0 || left < 0 || stamp_us < 0) return false;
    if (side != 0 && !known_side(side)) return false;
    if (!known_phase(phase)) return false;
    out.tag = tag;
    out.contra = contra;
    out.name = name;
    out.px = px;
    out.qty = qty;
    out.left = left;
    out.stamp_us = stamp_us;
    out.why = why;
    out.phase = phase;
    out.side = side;
    return true;
}

inline bool quote_make(Quote& out, uint32_t name, int64_t exch_us, int64_t local_us, int64_t bid_px, int64_t ask_px,
                       int64_t last_px, int64_t bid_qty, int64_t ask_qty, int64_t last_qty) noexcept {
    out = Quote{};
    if (name == 0 || exch_us < 0 || local_us < 0) return false;
    if (bid_px < 0 || ask_px < 0 || last_px < 0 || bid_qty < 0 || ask_qty < 0 || last_qty < 0) return false;
    if (bid_px > 0 && ask_px > 0 && bid_px > ask_px) return false;
    out.name = name;
    out.exch_us = exch_us;
    out.local_us = local_us;
    out.bid_px = bid_px;
    out.ask_px = ask_px;
    out.last_px = last_px;
    out.bid_qty = bid_qty;
    out.ask_qty = ask_qty;
    out.last_qty = last_qty;
    return true;
}

}  // namespace pitlane
