#pragma once

#include "pitlane/codec.hpp"
#include "pitlane/numeric.hpp"
#include "pitlane/plan.hpp"

#include <atomic>
#include <cstdint>

namespace pitlane {

// Close locks and the cash-sell lock. Open orders do not freeze inventory.

enum class LotErr : int8_t { Ok = 0, None = 1, NoToday = 2, NoYday = 3 };

struct LotLock {
    int64_t total{0};
    int64_t yday_left{0};
    int64_t locked{0};
    int64_t yday_locked{0};
    int64_t free_lots{0};
    int64_t buy_working{0};
    int64_t sell_working{0};
};

inline void add_working(LotLock& book, bool buy, int64_t target, int64_t locked_qty) noexcept {
    int64_t q = target < locked_qty ? target : locked_qty;
    if (q < 0) q = 0;
    if (buy) book.buy_working += q;
    else book.sell_working += q;
}

inline LotErr lock_future(LotLock& book, Off how, int64_t qty) noexcept {
    if (qty <= 0 || how == Off::Open) return LotErr::Ok;
    if (how == Off::Close) {
        if (book.total - book.locked < qty) return LotErr::None;
        book.locked += qty;
    } else if (how == Off::CloseToday) {
        const int64_t today = book.total - book.yday_left;
        const int64_t today_locked = book.locked - book.yday_locked;
        if (today - today_locked < qty) return LotErr::NoToday;
        book.locked += qty;
    } else if (how == Off::CloseYday) {
        if (book.yday_left - book.yday_locked < qty) return LotErr::NoYday;
        book.locked += qty;
        book.yday_locked += qty;
    } else {
        return LotErr::None;
    }
    book.free_lots = book.total - book.locked;
    return LotErr::Ok;
}

inline bool lock_cash_sell(LotLock& book, int64_t qty) noexcept {
    if (qty <= 0) return true;
    if (book.free_lots < qty) return false;
    book.free_lots -= qty;
    book.locked += qty;
    book.yday_locked += qty;
    return true;
}

inline bool basket_transfer(int64_t qty, int64_t px) noexcept { return qty > 0 && px == 0; }

struct StopBand {
    int64_t entry{0};
    int64_t stop{0};
    int64_t down_lim{0};
};

inline void arm_long_stop(StopBand& band, int64_t entry, int64_t room, int64_t down) noexcept {
    band.entry = entry;
    band.stop = entry - room;
    band.down_lim = down;
}

inline bool exit_long(const StopBand& band, int64_t px) noexcept {
    if (px <= band.stop) return true;
    if (band.down_lim > 0 && px <= band.down_lim) return true;
    return false;
}

struct StubOrder {
    int64_t qty{0};
    uint8_t buy{0};
};

inline StubOrder stub_flat(int64_t net, int64_t lot) noexcept {
    StubOrder out;
    if (net == 0) return out;
    if (lot <= 1) {
        out.qty = net < 0 ? -net : net;
        out.buy = net < 0 ? 1 : 0;
        return out;
    }
    if (net > 0) {
        out.qty = (net / lot) * lot;
        out.buy = 0;
        return out;
    }
    const int64_t need = -net;
    out.qty = ((need + lot - 1) / lot) * lot;
    out.buy = 1;
    return out;
}

struct UnitPurse {
    int64_t cash{0};
    int64_t qty{0};
};

struct PurseBook {
    int64_t cash{0};
    int64_t qty{0};
    UnitPurse unit[8]{};
    int n{0};
};

inline bool draw_unit(PurseBook& book, int unit, int64_t cash, int64_t qty) noexcept {
    if (unit < 0 || unit >= book.n || cash < 0 || qty < 0) return false;
    if (book.cash < cash || book.qty < qty) return false;
    if (book.unit[unit].cash < cash || book.unit[unit].qty < qty) return false;
    book.cash -= cash;
    book.qty -= qty;
    book.unit[unit].cash -= cash;
    book.unit[unit].qty -= qty;
    return true;
}

struct ClassScale {
    int64_t mult{1};
    int64_t margin_num{1};
    int64_t margin_den{1};
};

inline ClassScale scale_of(Product product) noexcept {
    if (cash_product(product)) return ClassScale{1, 1, 1};
    return ClassScale{100, 15, 100};
}

inline int64_t cash_charge(int64_t notional, int64_t commission, bool sell, int64_t stamp_num, int64_t xfer_num,
                           int64_t den) noexcept {
    if (den <= 0) return 0;
    const int64_t stamp = sell ? notional * stamp_num / den : 0;
    return notional * xfer_num / den + commission + stamp;
}

inline int64_t deriv_charge(int64_t notional, int64_t rate_num, int64_t den) noexcept {
    if (den <= 0) return 0;
    return notional * rate_num / den;
}

inline bool fill_lock(int64_t& lock_px, int64_t& lock_qty, int64_t qty, int64_t limit_px, int64_t bid,
                      int64_t ask) noexcept {
    if (lock_qty == 0) {
        lock_qty = qty;
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    if (lock_px != 0) return true;
    int64_t px = 0;
    if (limit_px != 0) px = limit_px;
    else if (bid > 0 && ask > 0) px = (bid + ask) / 2;
    else if (ask > 0) px = ask;
    else if (bid > 0) px = bid;
    else return false;
    lock_px = px;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return true;
}

inline int64_t margin_need(const ClassScale& scale, int64_t px, int64_t qty) noexcept {
    if (px <= 0 || qty <= 0 || scale.mult <= 0 || scale.margin_num < 0 || scale.margin_den <= 0) return 0;
    if (cmp_prod(scale.mult, px, INT64_MAX, 1) > 0) return INT64_MAX;
    const int64_t step = scale.mult * px;
    if (cmp_prod(step, qty, INT64_MAX, 1) > 0) return INT64_MAX;
    const int64_t notion = step * qty;
    if (scale.margin_num == 0) return 0;
    Wide w = mul_wide(static_cast<uint64_t>(notion), static_cast<uint64_t>(scale.margin_num));
    uint64_t q = 0;
    if (!div_wide(w, static_cast<uint64_t>(scale.margin_den), q)) return INT64_MAX;
    if (q > static_cast<uint64_t>(INT64_MAX)) return INT64_MAX;
    return static_cast<int64_t>(q);
}

inline int64_t charge_class(Product product, int64_t notional, int64_t commission, bool sell, int64_t stamp_num,
                            int64_t xfer_num, int64_t rate_num, int64_t den) noexcept {
    if (cash_product(product)) return cash_charge(notional, commission, sell, stamp_num, xfer_num, den);
    return deriv_charge(notional, rate_num, den);
}

struct LegLots {
    int64_t pos{0};
    int64_t today{0};
    int64_t yday{0};
    int64_t opened{0};
    int64_t closed{0};
};

struct ChaseBook {
    LegLots lng{};
    LegLots sht{};
    int64_t target{0};
    bool split_yday{false};
    int64_t open_cap{0};
};

inline bool open_fits(const ChaseBook& book, int64_t qty) noexcept {
    return qty > 0 && book.open_cap >= book.lng.opened + book.sht.opened + qty;
}

inline Move close_leg(bool split, bool sell, int64_t today, int64_t yday) noexcept {
    if (today + yday <= 0) return {};
    if (!split) return {sell ? MoveKind::SellClose : MoveKind::BuyClose, today + yday, 0};
    if (yday > 0) return {sell ? MoveKind::SellCloseYday : MoveKind::BuyCloseYday, yday, 0};
    return {sell ? MoveKind::SellCloseToday : MoveKind::BuyCloseToday, today, 0};
}

inline Move close_extra(bool split, bool sell, int64_t extra, int64_t yday) noexcept {
    if (extra <= 0) return {};
    if (!split) return {sell ? MoveKind::SellClose : MoveKind::BuyClose, extra, 0};
    if (yday == 0) return {sell ? MoveKind::SellCloseToday : MoveKind::BuyCloseToday, extra, 0};
    const int64_t q = extra < yday ? extra : yday;
    return {sell ? MoveKind::SellCloseYday : MoveKind::BuyCloseYday, q, 0};
}

inline Move commodity_plan(const ChaseBook& book) noexcept {
    if (book.target > 0) {
        if (book.sht.pos > 0) return close_leg(book.split_yday, false, book.sht.today, book.sht.yday);
        const int64_t extra = book.lng.pos - book.target;
        if (extra > 0) return close_extra(book.split_yday, true, extra, book.lng.yday);
        if (extra < 0 && open_fits(book, -extra)) return {MoveKind::BuyOpen, -extra, 0};
        return {};
    }
    if (book.target < 0) {
        if (book.lng.pos > 0) return close_leg(book.split_yday, true, book.lng.today, book.lng.yday);
        const int64_t want = -book.target;
        const int64_t extra = book.sht.pos - want;
        if (extra > 0) return close_extra(book.split_yday, false, extra, book.sht.yday);
        if (extra < 0 && open_fits(book, -extra)) return {MoveKind::SellOpen, -extra, 0};
        return {};
    }
    if (book.lng.pos > 0) return close_leg(book.split_yday, true, book.lng.today, book.lng.yday);
    if (book.sht.pos > 0) return close_leg(book.split_yday, false, book.sht.today, book.sht.yday);
    return {};
}

inline Move index_plan(const ChaseBook& book) noexcept {
    const int64_t net = book.lng.pos - book.sht.pos;
    const int64_t diff = book.target - net;
    if (diff > 0) {
        const int64_t yd = book.sht.pos - book.sht.opened;
        if (yd <= 0) {
            if (!open_fits(book, diff)) return {};
            return {MoveKind::BuyOpen, diff, 0};
        }
        return {MoveKind::BuyClose, yd >= diff ? diff : yd, 0};
    }
    if (diff < 0) {
        const int64_t need = -diff;
        const int64_t yd = book.lng.pos - book.lng.opened;
        if (yd <= 0) {
            if (!open_fits(book, need)) return {};
            return {MoveKind::SellOpen, need, 0};
        }
        return {MoveKind::SellClose, yd >= need ? need : yd, 0};
    }
    return {};
}

inline int64_t plan_px(MoveKind kind, int64_t bid, int64_t ask) noexcept {
    switch (kind) {
        case MoveKind::BuyOpen:
        case MoveKind::BuyClose:
        case MoveKind::BuyCloseToday:
        case MoveKind::BuyCloseYday:
            return ask;
        default:
            return bid;
    }
}

inline void book_open(LegLots& leg, int64_t qty, bool split) noexcept {
    if (qty <= 0) return;
    leg.opened += qty;
    leg.pos += qty;
    if (split) leg.today += qty;
}

inline void book_plain_close(LegLots& leg, int64_t qty) noexcept {
    if (qty <= 0) return;
    leg.closed += qty;
    leg.pos -= qty;
}

inline void book_close_today(LegLots& leg, int64_t qty) noexcept {
    if (qty <= 0) return;
    leg.closed += qty;
    leg.today -= qty;
    leg.pos = leg.today + leg.yday;
}

inline void book_close_yday(LegLots& leg, int64_t qty) noexcept {
    if (qty <= 0) return;
    leg.closed += qty;
    leg.yday -= qty;
    leg.pos = leg.today + leg.yday;
}

inline void book_fill(ChaseBook& book, MoveKind kind, int64_t qty) noexcept {
    switch (kind) {
        case MoveKind::BuyOpen: book_open(book.lng, qty, book.split_yday); break;
        case MoveKind::SellOpen: book_open(book.sht, qty, book.split_yday); break;
        case MoveKind::SellClose: book_plain_close(book.lng, qty); break;
        case MoveKind::BuyClose: book_plain_close(book.sht, qty); break;
        case MoveKind::SellCloseToday: book_close_today(book.lng, qty); break;
        case MoveKind::BuyCloseToday: book_close_today(book.sht, qty); break;
        case MoveKind::SellCloseYday: book_close_yday(book.lng, qty); break;
        case MoveKind::BuyCloseYday: book_close_yday(book.sht, qty); break;
        case MoveKind::None: break;
    }
}

struct FrontSeat {
    int32_t front{0};
    int32_t session{0};
    uint32_t next_ref{1};
};

inline void arm_front(FrontSeat& seat, int32_t front, int32_t session, uint32_t max_ref) noexcept {
    seat.front = front;
    seat.session = session;
    seat.next_ref = max_ref + 1;
}

inline uint32_t take_ref(FrontSeat& seat) noexcept { return seat.next_ref++; }

inline bool same_front(const FrontSeat& seat, int32_t front, int32_t session) noexcept {
    return seat.front == front && seat.session == session;
}

enum class QueueMark : uint8_t { InQueue = 1, Left = 2, Cancelled = 3 };

struct WorkSlot {
    uint32_t ref{0};
    int64_t ord_done{0};
    int64_t trd_done{0};
    int32_t pulls{0};
    uint8_t busy{0};
    uint8_t queued{0};
};

inline void on_order_echo(WorkSlot& slot, const FrontSeat& seat, int32_t front, int32_t session, uint32_t ref,
                          int64_t vol, QueueMark mark, bool changed) noexcept {
    if (!same_front(seat, front, session)) return;
    if (!slot.busy || ref != slot.ref) return;
    slot.ord_done = vol;
    if (!changed) return;
    if (mark == QueueMark::InQueue) {
        slot.queued = 1;
        return;
    }
    slot.queued = 0;
    if (mark == QueueMark::Cancelled) ++slot.pulls;
    if (slot.ord_done == slot.trd_done) slot.busy = 0;
}

inline void on_trade_echo(WorkSlot& slot, uint32_t ref, int64_t qty) noexcept {
    if (!slot.busy || ref != slot.ref || qty <= 0) return;
    slot.trd_done += qty;
    if (slot.queued) return;
    if (slot.ord_done == slot.trd_done) slot.busy = 0;
}

inline void on_reject_echo(WorkSlot& slot, uint32_t ref) noexcept {
    if (slot.busy && ref == slot.ref) slot.busy = 0;
}

struct MainPick {
    uint32_t name{0};
    int64_t vol{0};
    uint8_t held{0};
};

inline void keep_larger(MainPick& pick, uint32_t name, int64_t vol, bool in_trade) noexcept {
    if (!in_trade) return;
    if (!pick.held || vol > pick.vol) {
        pick.name = name;
        pick.vol = vol;
        pick.held = 1;
    }
}

inline bool wake_all(int err, int n, bool (*wake_at)(int index, void* user), void* user) noexcept {
    if (err != 0 || !wake_at || n < 0) return false;
    for (int i = 0; i < n; ++i) {
        if (!wake_at(i, user)) return false;
    }
    return true;
}

}  // namespace pitlane
