#include "pitlane/book/venue.hpp"

namespace pitlane {
namespace {

void sort_bids(Rung* a, int n) noexcept {
    for (int i = 1; i < n; ++i) {
        Rung key = a[i];
        int j = i;
        while (j > 0 && a[j - 1].px < key.px) {
            a[j] = a[j - 1];
            --j;
        }
        a[j] = key;
    }
}

void sort_asks(Rung* a, int n) noexcept {
    for (int i = 1; i < n; ++i) {
        Rung key = a[i];
        int j = i;
        while (j > 0 && a[j - 1].px > key.px) {
            a[j] = a[j - 1];
            --j;
        }
        a[j] = key;
    }
}

bool live_phase(uint8_t phase) noexcept {
    return phase == static_cast<uint8_t>(Phase::Staged) || phase == static_cast<uint8_t>(Phase::Working) ||
           phase == static_cast<uint8_t>(Phase::Partial) || phase == static_cast<uint8_t>(Phase::PullWait);
}

}  // namespace

PaperVenue::PaperVenue(const VenueCfg& cfg) noexcept : cfg_(cfg) {
    if (cfg_.lot <= 0) cfg_.lot = 1;
    if (cfg_.cancel_pow <= 0) cfg_.cancel_pow = 1;
    free_head_ = 0;
    for (uint32_t i = 0; i < kTickets - 1; ++i) free_next_[i] = static_cast<int>(i + 1);
    free_next_[kTickets - 1] = -1;
}

void PaperVenue::set_band(uint32_t name, int64_t down, int64_t up) noexcept {
    if (name >= kNames) return;
    names_[name].down = down;
    names_[name].up = up;
}

void PaperVenue::set_top(uint32_t name, int64_t bid_px, int64_t bid_qty, int64_t ask_px, int64_t ask_qty,
                         int64_t last_px) noexcept {
    if (name >= kNames) return;
    NameState& ns = names_[name];
    ns.bid_px = bid_px;
    ns.bid_qty = bid_qty;
    ns.ask_px = ask_px;
    ns.ask_qty = ask_qty;
    ns.last_px = last_px;
    ns.nbid = 0;
    ns.nask = 0;
    if (bid_px > 0 && bid_qty > 0) {
        ns.bids[0] = Rung{bid_px, bid_qty};
        ns.nbid = 1;
    }
    if (ask_px > 0 && ask_qty > 0) {
        ns.asks[0] = Rung{ask_px, ask_qty};
        ns.nask = 1;
    }
}

void PaperVenue::set_levels(uint32_t name, const Rung* bids, int nbid, const Rung* asks, int nask) noexcept {
    if (name >= kNames) return;
    NameState& ns = names_[name];
    ns.nbid = 0;
    ns.nask = 0;
    if (bids) {
        const int n = nbid > kDepth ? kDepth : nbid;
        for (int i = 0; i < n; ++i) ns.bids[i] = bids[i];
        sort_bids(ns.bids, n);
        ns.nbid = n;
        if (n > 0) {
            ns.bid_px = ns.bids[0].px;
            ns.bid_qty = ns.bids[0].qty;
        }
    }
    if (asks) {
        const int n = nask > kDepth ? kDepth : nask;
        for (int i = 0; i < n; ++i) ns.asks[i] = asks[i];
        sort_asks(ns.asks, n);
        ns.nask = n;
        if (n > 0) {
            ns.ask_px = ns.asks[0].px;
            ns.ask_qty = ns.asks[0].qty;
        }
    }
}

int PaperVenue::find_tag(uint64_t tag) const noexcept {
    if (tag == 0) return -1;
    for (uint32_t i = 0; i < kTickets; ++i) {
        if (tickets_[i].used && tickets_[i].tag == tag) return static_cast<int>(i);
    }
    return -1;
}

int PaperVenue::alloc_ticket() noexcept {
    const int id = free_head_;
    if (id < 0) return -1;
    free_head_ = free_next_[id];
    tickets_[id] = Ticket{};
    tickets_[id].used = 1;
    return id;
}

void PaperVenue::free_ticket(int i) noexcept {
    if (i < 0 || static_cast<uint32_t>(i) >= kTickets || !tickets_[i].used) return;
    tickets_[i].used = 0;
    free_next_[i] = free_head_;
    free_head_ = i;
}

int PaperVenue::emit(Report* buf, int cap, int n, const Report& r) const noexcept {
    if (!buf || n >= cap) return n;
    buf[n] = r;
    return n + 1;
}

int64_t PaperVenue::shown_at(const NameState& ns, uint8_t side, int64_t px) const noexcept {
    const Rung* row = buy_side(side) ? ns.bids : ns.asks;
    const int n = buy_side(side) ? ns.nbid : ns.nask;
    for (int i = 0; i < n; ++i) {
        if (row[i].px == px) return row[i].qty;
    }
    return 0;
}

bool PaperVenue::washes(const Intent& in) const noexcept {
    for (uint32_t i = 0; i < kTickets; ++i) {
        const Ticket& t = tickets_[i];
        if (!t.used || t.name != in.name || t.acct != in.acct || !live_phase(t.phase)) continue;
        if (t.side == in.side) continue;
        const int64_t left = t.qty - t.done;
        if (left <= 0) continue;
        if (buy_side(in.side) && in.px >= t.px) return true;
        if (sell_side(in.side) && in.px <= t.px) return true;
    }
    return false;
}

int PaperVenue::fill_ticket(int ti, int64_t px, int64_t qty, int64_t now_us, Report* buf, int cap, int n) noexcept {
    Ticket& t = tickets_[ti];
    int64_t room = t.qty - t.done;
    if (room < 0) room = 0;
    if (qty > room) qty = room;
    if (qty <= 0) return n;
    t.done += qty;
    t.phase = t.done >= t.qty ? static_cast<uint8_t>(Phase::Filled) : static_cast<uint8_t>(Phase::Partial);
    Report r{};
    r.tag = t.tag;
    r.px = px;
    r.qty = qty;
    r.left = t.qty - t.done;
    r.stamp_us = now_us;
    r.phase = t.phase;
    r.side = t.side;
    r.name = t.name;
    return emit(buf, cap, n, r);
}

int PaperVenue::activate(int ti, int64_t now_us, Report* buf, int cap, int n) noexcept {
    Ticket& t = tickets_[ti];
    const NameState& ns = names_[t.name];
    const auto style = static_cast<Style>(t.style);
    const int64_t leaves = t.qty - t.done;
    ArrivalHit hit = buy_side(t.side) ? walk_buy(style, t.px, leaves, ns.asks, ns.nask)
                                      : walk_sell(style, t.px, leaves, ns.bids, ns.nbid);
    if (hit.filled > 0) {
        int64_t px = t.px;
        const int64_t touch = buy_side(t.side) ? ns.ask_px : ns.bid_px;
        if (touch > 0 && ns.last_px > 0) px = median3(t.px, touch, ns.last_px);
        else if (touch > 0) px = touch;
        n = fill_ticket(ti, px, hit.filled, now_us, buf, cap, n);
    }
    if (hit.kind == Arrival::Rest) {
        t.phase = t.done > 0 ? static_cast<uint8_t>(Phase::Partial) : static_cast<uint8_t>(Phase::Working);
        gap_join(t.gap, shown_at(ns, t.side, t.px));
        Report r{};
        r.tag = t.tag;
        r.px = t.px;
        r.left = t.qty - t.done;
        r.stamp_us = now_us;
        r.phase = t.phase;
        r.side = t.side;
        r.name = t.name;
        return emit(buf, cap, n, r);
    }
    if (hit.kind == Arrival::Drop) {
        t.phase = static_cast<uint8_t>(Phase::Expired);
        Report r{};
        r.tag = t.tag;
        r.px = t.px;
        r.left = t.qty - t.done;
        r.stamp_us = now_us;
        r.phase = t.phase;
        r.side = t.side;
        r.name = t.name;
        n = emit(buf, cap, n, r);
        free_ticket(ti);
        return n;
    }
    if (t.used && t.done >= t.qty) free_ticket(ti);
    return n;
}

VenueOut PaperVenue::admit(const Intent& in, int64_t now_us, Report* buf, int cap) noexcept {
    VenueOut out;
    const bool pull = in.act == static_cast<uint8_t>(Act::Pull);
    if (!pull && in.qty <= 0) {
        out.why = Why::Qty;
        return out;
    }
    if (!pull && !known_side(in.side)) {
        out.why = Why::Side;
        return out;
    }
    if (in.tag == 0) {
        out.why = Why::Dup;
        return out;
    }
    if (in.stamp_us <= 0) {
        out.why = Why::Stamp;
        return out;
    }
    if (in.name >= kNames) {
        out.why = Why::Name;
        return out;
    }
    if (!pull && in.acct == 0) {
        out.why = Why::Acct;
        return out;
    }

    if (pull) {
        const int ti = find_tag(in.tag);
        if (ti < 0) {
            out.why = Why::Missing;
            return out;
        }
        Ticket& t = tickets_[ti];
        if (!live_phase(t.phase)) {
            out.why = Why::Missing;
            return out;
        }
        const int64_t lag = table_lag_us(cfg_.lag, minute_from_us(in.stamp_us), true);
        t.pull_ready_us = ready_at(in.stamp_us, lag);
        t.phase = static_cast<uint8_t>(Phase::PullWait);
        if (t.pull_ready_us <= now_us) {
            t.phase = static_cast<uint8_t>(Phase::Cancelled);
            Report r{};
            r.tag = t.tag;
            r.px = t.px;
            r.left = t.qty - t.done;
            r.qty = r.left;
            r.stamp_us = now_us;
            r.phase = t.phase;
            r.side = t.side;
            r.name = t.name;
            out.reports = emit(buf, cap, 0, r);
            free_ticket(ti);
            return out;
        }
        Report r{};
        r.tag = t.tag;
        r.px = t.px;
        r.left = t.qty - t.done;
        r.stamp_us = now_us;
        r.phase = t.phase;
        r.side = t.side;
        r.name = t.name;
        out.reports = emit(buf, cap, 0, r);
        return out;
    }

    if (in.style < 1 || in.style > 4) {
        out.why = Why::Mode;
        return out;
    }
    if (find_tag(in.tag) >= 0) {
        out.why = Why::Dup;
        return out;
    }
    NameState& ns = names_[in.name];
    if ((ns.up > 0 && in.px > ns.up) || (ns.down > 0 && in.px < ns.down)) {
        out.why = Why::Band;
        return out;
    }
    if (cfg_.notional_cap > 0 && in.px > 0 && in.qty > cfg_.notional_cap / in.px) {
        out.why = Why::Notional;
        return out;
    }
    if (cfg_.rate_cap > 0) {
        const int64_t sec = in.stamp_us / 1000000;
        if (ns.window_sec != sec) {
            ns.window_sec = sec;
            ns.admits = 0;
        }
        if (ns.admits >= cfg_.rate_cap) {
            out.why = Why::Rate;
            return out;
        }
    }
    if (washes(in)) {
        out.why = Why::Wash;
        return out;
    }
    const int ti = alloc_ticket();
    if (ti < 0) {
        out.why = Why::Full;
        return out;
    }
    if (cfg_.rate_cap > 0) ++ns.admits;

    Ticket& t = tickets_[ti];
    t.tag = in.tag;
    t.name = in.name;
    t.acct = in.acct;
    t.px = in.px;
    t.qty = in.qty;
    t.born_us = in.stamp_us;
    t.side = in.side;
    t.style = in.style;
    t.lot = in.lot > 0 ? in.lot : cfg_.lot;
    t.phase = static_cast<uint8_t>(Phase::Staged);
    const int64_t lag = table_lag_us(cfg_.lag, minute_from_us(in.stamp_us), false);
    t.ready_us = ready_at(in.stamp_us, lag);
    if (t.ready_us <= now_us) {
        out.reports = activate(ti, now_us, buf, cap, 0);
        return out;
    }
    Report r{};
    r.tag = t.tag;
    r.px = t.px;
    r.left = t.qty;
    r.stamp_us = now_us;
    r.phase = t.phase;
    r.side = t.side;
    r.name = t.name;
    out.reports = emit(buf, cap, 0, r);
    return out;
}

int PaperVenue::on_clock(int64_t now_us, Report* buf, int cap) noexcept {
    int n = 0;
    for (uint32_t i = 0; i < kTickets; ++i) {
        if (!tickets_[i].used) continue;
        if (tickets_[i].phase == static_cast<uint8_t>(Phase::Staged) && tickets_[i].ready_us <= now_us) {
            n = activate(static_cast<int>(i), now_us, buf, cap, n);
            continue;
        }
        if (tickets_[i].phase == static_cast<uint8_t>(Phase::PullWait) && tickets_[i].pull_ready_us <= now_us) {
            Ticket& t = tickets_[i];
            t.phase = static_cast<uint8_t>(Phase::Cancelled);
            Report r{};
            r.tag = t.tag;
            r.px = t.px;
            r.left = t.qty - t.done;
            r.qty = r.left;
            r.stamp_us = now_us;
            r.phase = t.phase;
            r.side = t.side;
            r.name = t.name;
            n = emit(buf, cap, n, r);
            free_ticket(static_cast<int>(i));
        }
    }
    return n;
}

int PaperVenue::on_print(uint32_t name, int64_t px, int64_t qty, int64_t now_us, Report* buf, int cap) noexcept {
    return on_tape(name, px, qty, 0, now_us, buf, cap);
}

int PaperVenue::on_tape(uint32_t name, int64_t px, int64_t qty, uint8_t aggressor, int64_t now_us, Report* buf,
                        int cap) noexcept {
    if (name >= kNames || qty <= 0) return 0;
    names_[name].last_px = px;
    int n = 0;
    for (uint32_t i = 0; i < kTickets; ++i) {
        Ticket& t = tickets_[i];
        if (!t.used || t.name != name) continue;
        if (aggressor != 0 && t.side == aggressor) continue;
        const uint8_t phase = t.phase;
        if (phase != static_cast<uint8_t>(Phase::Working) && phase != static_cast<uint8_t>(Phase::Partial) &&
            phase != static_cast<uint8_t>(Phase::PullWait))
            continue;
        const int64_t extra = cfg_.rigid_extra;
        const bool ok = buy_side(t.side) ? px <= t.px - extra : px >= t.px + extra;
        if (!ok) continue;
        const int64_t leaves = t.qty - t.done;
        int64_t got = 0;
        if (buy_side(t.side) && px < t.px) got = leaves;
        else if (sell_side(t.side) && px > t.px) got = leaves;
        else got = gap_print(t.gap, qty, t.lot, leaves);
        if (got <= 0) continue;
        n = fill_ticket(static_cast<int>(i), px, got, now_us, buf, cap, n);
        if (t.used && t.done >= t.qty) free_ticket(static_cast<int>(i));
    }
    return n;
}

int PaperVenue::on_shown(uint32_t name, uint8_t side, int64_t px, int64_t prev_qty, int64_t now_qty, int64_t now_us,
                         Report* buf, int cap) noexcept {
    if (name >= kNames || !known_side(side)) return 0;
    NameState& ns = names_[name];
    Rung* row = buy_side(side) ? ns.bids : ns.asks;
    int* nn = buy_side(side) ? &ns.nbid : &ns.nask;
    for (int i = 0; i < *nn; ++i) {
        if (row[i].px != px) continue;
        row[i].qty = now_qty;
        if (now_qty <= 0) {
            for (int j = i; j < *nn - 1; ++j) row[j] = row[j + 1];
            --(*nn);
        }
        break;
    }
    int n = 0;
    for (uint32_t i = 0; i < kTickets; ++i) {
        Ticket& t = tickets_[i];
        if (!t.used || t.name != name || t.side != side || t.px != px) continue;
        const uint8_t phase = t.phase;
        if (phase != static_cast<uint8_t>(Phase::Working) && phase != static_cast<uint8_t>(Phase::Partial) &&
            phase != static_cast<uint8_t>(Phase::PullWait))
            continue;
        const int64_t leaves = t.qty - t.done;
        const int64_t got = gap_resize(t.gap, prev_qty, now_qty, cfg_.cancel_pow, t.lot, leaves);
        if (got <= 0) continue;
        n = fill_ticket(static_cast<int>(i), px, got, now_us, buf, cap, n);
        if (t.used && t.done >= t.qty) free_ticket(static_cast<int>(i));
    }
    return n;
}

}  // namespace pitlane
