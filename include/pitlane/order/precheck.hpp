#pragma once

// Pre-trade rule pipeline for orders that reach the counter. It sits after the
// per-order RMS checks and before the OMS draft. The rules run in a fixed
// order and the first failure wins, so a rejection reason is always the same
// for the same state:
//
//   input -> halt -> open/close blacklist -> notional cap by class ->
//   account flow (1 s, 10 s) -> market flow (1 s) -> cancel ratio ->
//   cancelled-amount ratio -> self-trade
//
// Every rule reads caller-owned fixed arrays: no allocation, no locks. Active
// orders are held in a 128-slot table; when it is full the gate fails closed
// instead of silently dropping self-trade cover.

#include "pitlane/desk/types.hpp"
#include "pitlane/order/flowgate.hpp"

#include <cstdint>

namespace pitlane {

enum class PreCode : uint8_t {
    Ok = 0,
    BadInput,
    Halted,
    OpenBlacklist,
    CloseBlacklist,
    Notional,
    AcctFlow1s,
    AcctFlow10s,
    MarketFlow1s,
    CancelRatio,
    AmountCancelRatio,
    SelfTrade,
    ActiveFull,
};

inline const char* pre_code_name(PreCode c) noexcept {
    switch (c) {
        case PreCode::Ok: return "ok";
        case PreCode::BadInput: return "bad input";
        case PreCode::Halted: return "halted";
        case PreCode::OpenBlacklist: return "open blacklist";
        case PreCode::CloseBlacklist: return "close blacklist";
        case PreCode::Notional: return "notional cap";
        case PreCode::AcctFlow1s: return "account orders 1s";
        case PreCode::AcctFlow10s: return "account orders 10s";
        case PreCode::MarketFlow1s: return "market orders 1s";
        case PreCode::CancelRatio: return "cancel ratio";
        case PreCode::AmountCancelRatio: return "cancel amount ratio";
        case PreCode::SelfTrade: return "self trade";
        case PreCode::ActiveFull: return "active table full";
    }
    return "rejected";
}

// Zero always means "rule off". Notional caps are indexed by a caller class:
// slot 0 is the default and slots 1..7 hold per-product overrides.
struct PreLimits {
    int64_t max_notional[8]{};
    int32_t max_orders_1s{0};
    int32_t max_orders_10s{0};
    int32_t max_market_orders_1s{0};
    int32_t max_cancel_ratio_ppm{0};
    int32_t min_orders_for_ratio{0};
    int32_t max_cancel_amount_ratio_ppm{0};
    int32_t cancel_amount_window_s{10};
    uint8_t self_trade_block{0};
};

// Sorted, fixed-size name list. A duplicate add is a no-op that still succeeds.
class NameSet {
public:
    static constexpr int kCap = 64;

    void clear() noexcept { n_ = 0; }

    bool add(uint32_t name) noexcept {
        if (name == 0 || n_ >= kCap) return false;
        int lo = 0;
        int hi = n_;
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (names_[mid] < name) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < n_ && names_[lo] == name) return true;
        for (int i = n_; i > lo; --i) names_[i] = names_[i - 1];
        names_[lo] = name;
        ++n_;
        return true;
    }

    bool has(uint32_t name) const noexcept {
        if (name == 0) return false;
        int lo = 0;
        int hi = n_;
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (names_[mid] == name) return true;
            if (names_[mid] < name) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return false;
    }

    int size() const noexcept { return n_; }
    uint32_t at(int i) const noexcept { return i >= 0 && i < n_ ? names_[i] : 0; }

private:
    uint32_t names_[kCap]{};
    int n_{0};
};

struct PreReq {
    uint32_t acct{0};
    uint32_t name{0};
    uint8_t market{0};
    uint8_t cls{0};   // caller class into PreLimits::max_notional
    uint8_t side{0};  // Side
    uint8_t is_open{1};
    uint8_t rest{1};  // the order may rest, so it can self-trade later
    int64_t px{0};
    int64_t qty{0};
    int64_t now_us{0};
};

struct PreActive {
    uint64_t id{0};
    uint32_t acct{0};
    uint32_t name{0};
    int64_t px{0};
    int64_t qty{0};
    uint8_t side{0};
    uint8_t used{0};
};

class PreTradeGuard {
public:
    static constexpr int kActive = 128;

    const PreLimits& limits() const noexcept { return lim_; }
    void set_limits(const PreLimits& l) noexcept { lim_ = l; }

    NameSet& open_blacklist() noexcept { return open_; }
    const NameSet& open_blacklist() const noexcept { return open_; }
    NameSet& close_blacklist() noexcept { return close_; }
    const NameSet& close_blacklist() const noexcept { return close_; }

    void halt(bool on) noexcept { halted_ = on ? 1 : 0; }
    bool halted() const noexcept { return halted_ != 0; }

    FlowGate& flow() noexcept { return flow_; }
    const FlowGate& flow() const noexcept { return flow_; }

    // Clears counters and the active table. Limits and lists stay.
    void reset() noexcept {
        flow_.reset();
        for (int i = 0; i < kActive; ++i) active_[i] = PreActive{};
        halted_ = 0;
    }

    // `clash` receives the id of the resting order that shares the account and
    // the instrument on the opposite side at a crossing price.
    PreCode check(const PreReq& r, uint64_t* clash = nullptr) const noexcept {
        if (clash) *clash = 0;
        if (halted_ != 0) return PreCode::Halted;
        if (r.acct >= FlowGate::kAccounts || r.name == 0 || r.px <= 0 || r.qty <= 0 || !known_side(r.side)) {
            return PreCode::BadInput;
        }
        if (r.is_open != 0) {
            if (open_.has(r.name)) return PreCode::OpenBlacklist;
        } else if (close_.has(r.name)) {
            return PreCode::CloseBlacklist;
        }
        const int cls = r.cls < 8 ? static_cast<int>(r.cls) : 0;
        int64_t cap = lim_.max_notional[cls];
        if (cap <= 0) cap = lim_.max_notional[0];
        if (cap > 0) {
            if (r.px > INT64_MAX / r.qty) return PreCode::Notional;
            if (r.px * r.qty > cap) return PreCode::Notional;
        }
        if (lim_.max_orders_1s > 0 && flow_.orders(r.acct, 1, r.now_us) >= lim_.max_orders_1s) {
            return PreCode::AcctFlow1s;
        }
        if (lim_.max_orders_10s > 0 && flow_.orders(r.acct, 10, r.now_us) >= lim_.max_orders_10s) {
            return PreCode::AcctFlow10s;
        }
        if (lim_.max_market_orders_1s > 0 &&
            flow_.market_orders(r.market, 1, r.now_us) >= lim_.max_market_orders_1s) {
            return PreCode::MarketFlow1s;
        }
        if (lim_.max_cancel_ratio_ppm > 0) {
            const int64_t placed = flow_.lifetime_orders(r.acct);
            if (lim_.min_orders_for_ratio <= 0 || placed >= lim_.min_orders_for_ratio) {
                const int64_t ppm = (flow_.lifetime_cancels(r.acct) + 1) * 1000000 / (placed + 1);
                if (ppm > lim_.max_cancel_ratio_ppm) return PreCode::CancelRatio;
            }
        }
        if (lim_.max_cancel_amount_ratio_ppm > 0) {
            const int32_t win = lim_.cancel_amount_window_s > 0 ? lim_.cancel_amount_window_s : 10;
            const int64_t placed = flow_.notional(r.acct, win, r.now_us);
            if (placed > 0) {
                const int64_t canceled = flow_.canceled(r.acct, win, r.now_us);
                if (canceled * 1000000 / placed > lim_.max_cancel_amount_ratio_ppm) {
                    return PreCode::AmountCancelRatio;
                }
            }
        }
        if (lim_.self_trade_block != 0) {
            if (r.rest != 0 && active_count() >= kActive) return PreCode::ActiveFull;
            const uint64_t id = find_clash(r);
            if (id != 0) {
                if (clash) *clash = id;
                return PreCode::SelfTrade;
            }
        }
        return PreCode::Ok;
    }

    // Records an accepted order and, when it can rest, its place in the active
    // table. Returns false when the table is full; the caller must then drop
    // the order because self-trade cover would be incomplete.
    bool on_submit(const PreReq& r, uint64_t id) noexcept {
        if (r.acct >= FlowGate::kAccounts) return false;
        int64_t notional = 0;
        if (r.px > 0 && r.qty > 0) notional = r.px > INT64_MAX / r.qty ? INT64_MAX : r.px * r.qty;
        flow_.on_order(r.acct, r.market, notional, r.now_us);
        if (r.rest == 0 || id == 0) return true;
        return add_active(r, id);
    }

    void on_cancel(uint32_t acct, uint8_t market, int64_t px, int64_t qty, int64_t now_us) noexcept {
        int64_t notional = 0;
        if (px > 0 && qty > 0) notional = px > INT64_MAX / qty ? INT64_MAX : px * qty;
        flow_.on_cancel(acct, market, notional, now_us);
    }

    void on_done(uint64_t id) noexcept {
        if (id == 0) return;
        for (int i = 0; i < kActive; ++i) {
            if (active_[i].used && active_[i].id == id) {
                active_[i] = PreActive{};
                return;
            }
        }
    }

    bool tracking(uint64_t id) const noexcept {
        if (id == 0) return false;
        for (int i = 0; i < kActive; ++i) {
            if (active_[i].used && active_[i].id == id) return true;
        }
        return false;
    }

    int active_count() const noexcept {
        int n = 0;
        for (int i = 0; i < kActive; ++i) n += active_[i].used ? 1 : 0;
        return n;
    }

    const PreActive* active(int i) const noexcept {
        if (i < 0 || i >= kActive || !active_[i].used) return nullptr;
        return &active_[i];
    }

private:
    bool add_active(const PreReq& r, uint64_t id) noexcept {
        for (int i = 0; i < kActive; ++i) {
            if (!active_[i].used) {
                active_[i].used = 1;
                active_[i].id = id;
                active_[i].acct = r.acct;
                active_[i].name = r.name;
                active_[i].px = r.px;
                active_[i].qty = r.qty;
                active_[i].side = r.side;
                return true;
            }
        }
        return false;
    }

    uint64_t find_clash(const PreReq& r) const noexcept {
        const bool buy = r.side == static_cast<uint8_t>(Side::Buy);
        for (int i = 0; i < kActive; ++i) {
            const PreActive& a = active_[i];
            if (!a.used || a.acct != r.acct || a.name != r.name) continue;
            const bool opposite = buy ? a.side == static_cast<uint8_t>(Side::Sell)
                                      : a.side == static_cast<uint8_t>(Side::Buy);
            if (!opposite) continue;
            if (buy ? a.px <= r.px : a.px >= r.px) return a.id;
        }
        return 0;
    }

    PreLimits lim_{};
    NameSet open_{};
    NameSet close_{};
    FlowGate flow_{};
    PreActive active_[kActive]{};
    uint8_t halted_{0};
};

}  // namespace pitlane
