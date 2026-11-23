#include "pitlane/book/venue.hpp"

#include "pitlane/book/touch.hpp"
#include "pitlane/desk/codec.hpp"

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
    for (uint32_t i = 0; i < kNames; ++i) {
        name_head_[i] = -1;
        name_tail_[i] = -1;
    }
}

void PaperVenue::set_band(uint32_t name, int64_t down, int64_t up) noexcept {
    if (name >= kNames) return;
    if (down > 0 && up > 0 && down > up) return;
    names_[name].down = down;
    names_[name].up = up;
}

void PaperVenue::set_top(uint32_t name, int64_t bid_px, int64_t bid_qty, int64_t ask_px, int64_t ask_qty,
                         int64_t last_px) noexcept {
    if (name >= kNames) return;
    NameState& ns = names_[name];
    // A top-of-book update keeps deeper levels only when the best price is unchanged.
    // A new best price invalidates the old ladder, so that side is rebuilt.
    const int old_nbid = ns.nbid;
    const int old_nask = ns.nask;
    const int64_t old_bid0 = old_nbid > 0 ? ns.bids[0].px : 0;
    const int64_t old_ask0 = old_nask > 0 ? ns.asks[0].px : 0;
    ns.bid_px = bid_px;
    ns.bid_qty = bid_qty;
    ns.ask_px = ask_px;
    ns.ask_qty = ask_qty;
    ns.last_px = last_px;
    if (bid_px > 0 && bid_qty > 0) {
        if (old_nbid > 0 && old_bid0 == bid_px) {
            ns.bids[0].qty = bid_qty;
            ns.nbid = old_nbid;
        } else {
            ns.bids[0] = Rung{bid_px, bid_qty};
            ns.nbid = 1;
        }
    } else {
        ns.nbid = 0;
    }
    if (ask_px > 0 && ask_qty > 0) {
        if (old_nask > 0 && old_ask0 == ask_px) {
            ns.asks[0].qty = ask_qty;
            ns.nask = old_nask;
        } else {
            ns.asks[0] = Rung{ask_px, ask_qty};
            ns.nask = 1;
        }
    } else {
        ns.nask = 0;
    }
}

void PaperVenue::set_levels(uint32_t name, const Rung* bids, int nbid, const Rung* asks, int nask) noexcept {
    if (name >= kNames) return;
    NameState& ns = names_[name];
    const auto load = [](const Rung* src, int nsrc, Rung* dst) -> int {
        int n = 0;
        if (!src || nsrc <= 0) return 0;
        const int m = nsrc > static_cast<int>(kDepth) ? static_cast<int>(kDepth) : nsrc;
        for (int i = 0; i < m; ++i) {
            if (src[i].px <= 0 || src[i].qty <= 0) continue;
            int j = 0;
            while (j < n && dst[j].px != src[i].px) ++j;
            if (j < n) dst[j].qty += src[i].qty;
            else dst[n++] = src[i];
        }
        return n;
    };
    ns.nbid = load(bids, nbid, ns.bids);
    if (ns.nbid > 0) {
        sort_bids(ns.bids, ns.nbid);
        ns.bid_px = ns.bids[0].px;
        ns.bid_qty = ns.bids[0].qty;
    } else {
        ns.bid_px = 0;
        ns.bid_qty = 0;
    }
    ns.nask = load(asks, nask, ns.asks);
    if (ns.nask > 0) {
        sort_asks(ns.asks, ns.nask);
        ns.ask_px = ns.asks[0].px;
        ns.ask_qty = ns.asks[0].qty;
    } else {
        ns.ask_px = 0;
        ns.ask_qty = 0;
    }
}

uint32_t PaperVenue::tag_mix(uint64_t tag) noexcept {
    tag ^= tag >> 30;
    tag *= 0xbf58476d1ce4e5b9ull;
    tag ^= tag >> 27;
    return static_cast<uint32_t>(tag) & (kTagMap - 1u);
}

int PaperVenue::find_tag(uint64_t tag) const noexcept {
    if (tag == 0) return -1;
    uint32_t i = tag_mix(tag);
    for (uint32_t n = 0; n < kTagMap; ++n) {
        const TagRef& s = tag_[i];
        if (s.state == 0) return -1;
        if (s.state == 1 && s.tag == tag) return s.idx;
        i = (i + 1u) & (kTagMap - 1u);
    }
    return -1;
}

bool PaperVenue::tag_put(int idx, uint64_t tag) noexcept {
    if (tag == 0 || idx < 0) return false;
    uint32_t i = tag_mix(tag);
    int tomb = -1;
    for (uint32_t n = 0; n < kTagMap; ++n) {
        TagRef& s = tag_[i];
        if (s.state == 1 && s.tag == tag) return false;
        if (s.state == 2 && tomb < 0) tomb = static_cast<int>(i);
        if (s.state == 0) {
            const uint32_t at = tomb >= 0 ? static_cast<uint32_t>(tomb) : i;
            tag_[at].tag = tag;
            tag_[at].idx = idx;
            tag_[at].state = 1;
            return true;
        }
        i = (i + 1u) & (kTagMap - 1u);
    }
    if (tomb < 0) return false;
    tag_[tomb].tag = tag;
    tag_[tomb].idx = idx;
    tag_[tomb].state = 1;
    return true;
}

void PaperVenue::tag_del(uint64_t tag) noexcept {
    if (tag == 0) return;
    uint32_t i = tag_mix(tag);
    for (uint32_t n = 0; n < kTagMap; ++n) {
        TagRef& s = tag_[i];
        if (s.state == 0) return;
        if (s.state == 1 && s.tag == tag) {
            s.state = 2;
            s.tag = 0;
            s.idx = -1;
            return;
        }
        i = (i + 1u) & (kTagMap - 1u);
    }
}

void PaperVenue::due_unlink(int i) noexcept {
    if (!due_on_[i]) return;
    due_on_[i] = 0;
    const int prev = due_prev_[i];
    const int next = due_next_[i];
    if (prev >= 0) due_next_[prev] = next;
    else due_head_ = next;
    if (next >= 0) due_prev_[next] = prev;
    else due_tail_ = prev;
}

// Ready time decides the order in which staged and cancel-pending tickets fire.
int64_t PaperVenue::due_key(int i) const noexcept {
    const Ticket& t = tickets_[i];
    if (t.phase == static_cast<uint8_t>(Phase::Staged)) return t.ready_us;
    if (t.phase == static_cast<uint8_t>(Phase::PullWait)) return t.pull_ready_us;
    return 0;
}

void PaperVenue::due_update(int i) noexcept {
    Ticket& t = tickets_[i];
    if (t.sticky || !timed_phase(t.phase)) {
        due_unlink(i);
        return;
    }
    const int64_t key = due_key(i);
    if (key <= 0) {
        due_unlink(i);
        return;
    }
    if (due_on_[i]) due_unlink(i);
    due_on_[i] = 1;
    due_prev_[i] = -1;
    due_next_[i] = -1;
    int prev = -1;
    int cur = due_head_;
    while (cur >= 0) {
        const int64_t other = due_key(cur);
        if (other > key || (other == key && cur > i)) break;
        prev = cur;
        cur = due_next_[cur];
    }
    due_prev_[i] = prev;
    due_next_[i] = cur;
    if (prev >= 0) due_next_[prev] = i;
    else due_head_ = i;
    if (cur >= 0) due_prev_[cur] = i;
    else due_tail_ = i;
}

void PaperVenue::live_link(int i) noexcept {
    // Per-name live list in ascending slot order, which keeps print matching
    // slot-ordered. Ids normally arrive in ascending order, so the two ends cover
    // nearly every admit without a scan.
    const uint32_t nm = tickets_[i].name;
    const int head = name_head_[nm];
    if (head < 0) {
        name_head_[nm] = i;
        name_tail_[nm] = i;
        name_prev_[i] = -1;
        name_next_[i] = -1;
    } else if (i > name_tail_[nm]) {
        name_prev_[i] = name_tail_[nm];
        name_next_[i] = -1;
        name_next_[name_tail_[nm]] = i;
        name_tail_[nm] = i;
    } else if (i < head) {
        name_prev_[i] = -1;
        name_next_[i] = head;
        name_prev_[head] = i;
        name_head_[nm] = i;
    } else {
        int prev = -1;
        int cur = head;
        while (cur >= 0 && cur < i) {
            prev = cur;
            cur = name_next_[cur];
        }
        name_prev_[i] = prev;
        name_next_[i] = cur;
        if (prev >= 0) name_next_[prev] = i;
        if (cur >= 0) name_prev_[cur] = i;
        else name_tail_[nm] = i;
    }
}

void PaperVenue::live_unlink(int i) noexcept {
    const uint32_t nm = tickets_[i].name;
    const int nprev = name_prev_[i];
    const int nnext = name_next_[i];
    if (nprev >= 0) name_next_[nprev] = nnext;
    else name_head_[nm] = nnext;
    if (nnext >= 0) name_prev_[nnext] = nprev;
    else name_tail_[nm] = nprev;
}

int PaperVenue::alloc_ticket(uint32_t name) noexcept {
    const int id = free_head_;
    if (id < 0) return -1;
    free_head_ = free_next_[id];
    tickets_[id] = Ticket{};
    tickets_[id].used = 1;
    tickets_[id].name = name;
    live_link(id);
    return id;
}

void PaperVenue::free_ticket(int i) noexcept {
    if (i < 0 || static_cast<uint32_t>(i) >= kTickets || !tickets_[i].used) return;
    // A ticket can be freed while still waiting (bad stamp, tag map full); make sure
    // it leaves the pending list so on_clock cannot activate a dead order.
    due_unlink(i);
    tag_del(tickets_[i].tag);
    live_unlink(i);
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
    for (int i = name_head_[in.name]; i >= 0; i = name_next_[i]) {
        const Ticket& t = tickets_[i];
        if (t.acct != in.acct || !live_phase(t.phase)) continue;
        if (t.side == in.side) continue;
        const int64_t left = t.qty - t.done;
        if (left <= 0) continue;
        if (buy_side(in.side) && in.px >= t.px) return true;
        if (sell_side(in.side) && in.px <= t.px) return true;
    }
    return false;
}

int PaperVenue::fill_ticket(int ti, int64_t px, int64_t qty, int64_t now_us, Report* buf, int cap, int n) noexcept {
    if (buf && n >= cap) return n;
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
    r.cause = t.cause;
    r.biz = t.biz;
    r.name = t.name;
    return emit(buf, cap, n, r);
}

int PaperVenue::activate(int ti, int64_t now_us, Report* buf, int cap, int n) noexcept {
    Ticket& t = tickets_[ti];
    due_unlink(ti);
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
        r.cause = t.cause;
        r.biz = t.biz;
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
        r.cause = t.cause;
        r.biz = t.biz;
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
    const Biz biz = static_cast<Biz>(in.biz);
    const bool directional = biz_directional(biz);
    if (!biz_known(biz)) {
        out.why = Why::Biz;
        return out;
    }
    if (directional && in.act != static_cast<uint8_t>(Act::Enter) &&
        in.act != static_cast<uint8_t>(Act::Pull)) {
        out.why = Why::Mode;
        return out;
    }
    if (!pull && !cfg_.trade_open) {
        out.why = Why::Closed;
        return out;
    }
    if (!pull && in.qty <= 0) {
        out.why = Why::Qty;
        return out;
    }
    if (!pull && directional && !known_side(in.side)) {
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
        int64_t lag = 0;
        if (cfg_.clock && cfg_.clock->npts > 0) {
            lag = cfg_.clock->back(in.stamp_us);
            if (lag < 0) {
                out.why = Why::Stamp;
                return out;
            }
        } else {
            lag = table_lag_us(cfg_.lag, minute_from_us(in.stamp_us), true);
        }
        t.pull_ready_us = ready_at(in.stamp_us, lag);
        t.phase = static_cast<uint8_t>(Phase::PullWait);
        due_update(ti);
        if (t.pull_ready_us <= now_us) {
            t.phase = static_cast<uint8_t>(Phase::Cancelled);
            due_update(ti);
            Report r{};
            r.tag = t.tag;
            r.px = t.px;
            r.left = t.qty - t.done;
            r.qty = r.left;
            r.stamp_us = now_us;
            r.phase = t.phase;
            r.side = t.side;
            r.cause = t.cause;
            r.biz = t.biz;
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
        r.cause = t.cause;
        r.biz = t.biz;
        r.name = t.name;
        out.reports = emit(buf, cap, 0, r);
        return out;
    }

    if (directional && (in.style < 1 || in.style > 5)) {
        out.why = Why::Mode;
        return out;
    }
    if (find_tag(in.tag) >= 0) {
        out.why = Why::Dup;
        return out;
    }
    if (!directional) {
        // Local business posting: collateral, lock, repayment, split, or merge.
        // There is no book side, so the venue records it as complete at once.
        Report r{};
        r.tag = in.tag;
        r.qty = in.qty;
        r.stamp_us = now_us;
        r.phase = static_cast<uint8_t>(Phase::Filled);
        r.side = in.side;
        r.cause = in.force_reason;
        r.biz = in.biz;
        r.name = in.name;
        out.reports = emit(buf, cap, 0, r);
        return out;
    }
    NameState& ns = names_[in.name];
    Intent ord = in;
    if (ord.px_mode > static_cast<int8_t>(PxMode::Limit)) {
        const int64_t px = resolve_px(static_cast<PxMode>(ord.px_mode), ord.side, ord.px, ns.bid_px, ns.ask_px);
        if (px <= 0) {
            out.why = Why::Book;
            return out;
        }
        ord.px = px;
    }
    if (ord.px <= 0) {
        out.why = Why::Book;
        return out;
    }
    if ((ns.up > 0 && ord.px > ns.up) || (ns.down > 0 && ord.px < ns.down)) {
        out.why = Why::Band;
        return out;
    }
    if (cfg_.notional_cap > 0 && ord.px > 0 && ord.qty > cfg_.notional_cap / ord.px) {
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
    if (washes(ord)) {
        out.why = Why::Wash;
        return out;
    }
    const int ti = alloc_ticket(in.name);
    if (ti < 0) {
        out.why = Why::Full;
        return out;
    }
    if (cfg_.rate_cap > 0) ++ns.admits;

    Ticket& t = tickets_[ti];
    t.tag = in.tag;
    if (!tag_put(ti, in.tag)) {
        free_ticket(ti);
        out.why = Why::Full;
        return out;
    }
    t.acct = in.acct;
    t.px = ord.px;
    t.qty = in.qty;
    t.born_us = in.stamp_us;
    t.side = in.side;
    t.style = in.style;
    t.cause = in.force_reason;
    t.biz = in.biz;
    t.lot = in.lot > 0 ? in.lot : cfg_.lot;
    t.phase = static_cast<uint8_t>(Phase::Staged);
    int64_t lag = 0;
    if (cfg_.clock && cfg_.clock->npts > 0) {
        lag = cfg_.clock->go(in.stamp_us);
        if (lag < 0) {
            free_ticket(ti);
            out.why = Why::Stamp;
            return out;
        }
    } else {
        lag = table_lag_us(cfg_.lag, minute_from_us(in.stamp_us), false);
    }
    t.ready_us = ready_at(in.stamp_us, lag);
    due_update(ti);
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
    r.cause = t.cause;
    r.biz = t.biz;
    r.name = t.name;
    out.reports = emit(buf, cap, 0, r);
    return out;
}

int PaperVenue::on_clock(int64_t now_us, Report* buf, int cap) noexcept {
    if (due_head_ < 0) return 0;
    int n = 0;
    for (int i = due_head_; i >= 0;) {
        const int next = due_next_[i];
        const int64_t key = due_key(i);
        if (key <= 0) {
            due_unlink(i);
            i = next;
            continue;
        }
        if (key > now_us) break;
        if (buf && n >= cap) break;
        Ticket& t = tickets_[i];
        if (t.phase == static_cast<uint8_t>(Phase::Staged) && t.ready_us <= now_us) {
            n = activate(i, now_us, buf, cap, n);
        } else if (t.phase == static_cast<uint8_t>(Phase::PullWait) && t.pull_ready_us <= now_us) {
            t.phase = static_cast<uint8_t>(Phase::Cancelled);
            due_unlink(i);
            Report r{};
            r.tag = t.tag;
            r.px = t.px;
            r.left = t.qty - t.done;
            r.qty = r.left;
            r.stamp_us = now_us;
            r.phase = t.phase;
            r.side = t.side;
            r.cause = t.cause;
            r.biz = t.biz;
            r.name = t.name;
            n = emit(buf, cap, n, r);
            free_ticket(i);
        }
        i = next;
    }
    return n;
}

int PaperVenue::fill_silent(int ti, int64_t qty) noexcept {
    Ticket& t = tickets_[ti];
    int64_t room = t.qty - t.done;
    if (room < 0) room = 0;
    if (qty > room) qty = room;
    if (qty <= 0) return 0;
    t.done += qty;
    t.phase = t.done >= t.qty ? static_cast<uint8_t>(Phase::Filled) : static_cast<uint8_t>(Phase::Partial);
    due_update(ti);
    if (t.done >= t.qty) free_ticket(ti);
    return 1;
}

int PaperVenue::force_fill(uint64_t tag, int64_t px, int64_t qty, int64_t now_us, Report* buf, int cap) noexcept {
    const int ti = find_tag(tag);
    if (ti < 0) return -1;
    Ticket& t = tickets_[ti];
    if (!live_phase(t.phase) && t.phase != static_cast<uint8_t>(Phase::Staged)) return -1;
    t.sticky = 0;
    if (t.phase == static_cast<uint8_t>(Phase::Staged)) t.phase = static_cast<uint8_t>(Phase::Working);
    const int64_t fill_px = px > 0 ? px : t.px;
    int64_t want = qty > 0 ? qty : (t.qty - t.done);
    const int n = fill_ticket(ti, fill_px, want, now_us, buf, cap, 0);
    due_update(ti);
    if (tickets_[ti].used && tickets_[ti].done >= tickets_[ti].qty) free_ticket(ti);
    return n;
}

int PaperVenue::force_pull(uint64_t tag, int64_t now_us, Report* buf, int cap) noexcept {
    const int ti = find_tag(tag);
    if (ti < 0) return -1;
    Ticket& t = tickets_[ti];
    if (!live_phase(t.phase) && t.phase != static_cast<uint8_t>(Phase::Staged)) return -1;
    t.sticky = 0;
    t.phase = static_cast<uint8_t>(Phase::Cancelled);
    due_update(ti);
    Report r{};
    r.tag = t.tag;
    r.px = t.px;
    r.left = t.qty - t.done;
    r.qty = r.left;
    r.stamp_us = now_us;
    r.phase = t.phase;
    r.side = t.side;
    r.cause = t.cause;
    r.biz = t.biz;
    r.name = t.name;
    const int n = emit(buf, cap, 0, r);
    free_ticket(ti);
    return n;
}

int PaperVenue::force_deny(uint64_t tag, int64_t now_us, Report* buf, int cap) noexcept {
    const int ti = find_tag(tag);
    if (ti < 0) return -1;
    Ticket& t = tickets_[ti];
    t.sticky = 0;
    t.phase = static_cast<uint8_t>(Phase::Denied);
    due_update(ti);
    Report r{};
    r.tag = t.tag;
    r.px = t.px;
    r.left = t.qty - t.done;
    r.stamp_us = now_us;
    r.phase = t.phase;
    r.side = t.side;
    r.cause = t.cause;
    r.biz = t.biz;
    r.name = t.name;
    r.why = static_cast<uint16_t>(Why::Mode);
    const int n = emit(buf, cap, 0, r);
    free_ticket(ti);
    return n;
}

bool PaperVenue::hang(uint64_t tag, uint8_t on) noexcept {
    const int ti = find_tag(tag);
    if (ti < 0) return false;
    tickets_[ti].sticky = on ? 1 : 0;
    due_update(ti);
    return true;
}

int PaperVenue::drill_apply(const DrillRow& row, int64_t now_us, Report* buf, int cap) noexcept {
    if (row.tag == 0 || row.cue == DrillCue::None) return -1;
    switch (row.cue) {
        case DrillCue::FillAll:
            return force_fill(row.tag, row.px, 0, now_us, buf, cap);
        case DrillCue::FillTwice: {
            const int ti = find_tag(row.tag);
            if (ti < 0) return -1;
            Ticket& t = tickets_[ti];
            const int64_t leaves = t.qty - t.done;
            if (leaves <= 0) return -1;
            const int64_t first = leaves / 2 > 0 ? leaves / 2 : 1;
            int n = force_fill(row.tag, row.px, first, now_us, buf, cap);
            if (n < 0) return n;
            if (find_tag(row.tag) >= 0) {
                const int n2 = force_fill(row.tag, row.px, 0, now_us, buf ? buf + n : nullptr, cap - n);
                if (n2 > 0) n += n2;
            }
            return n;
        }
        case DrillCue::Kill:
            return force_pull(row.tag, now_us, buf, cap);
        case DrillCue::KillLeft: {
            const int ti = find_tag(row.tag);
            if (ti < 0) return -1;
            Ticket& t = tickets_[ti];
            int64_t part = row.qty > 0 ? row.qty : (t.qty - t.done) / 2;
            if (part <= 0) part = 1;
            int n = force_fill(row.tag, row.px, part, now_us, buf, cap);
            if (n < 0) return n;
            if (find_tag(row.tag) >= 0) {
                const int n2 = force_pull(row.tag, now_us, buf ? buf + n : nullptr, cap - n);
                if (n2 > 0) n += n2;
            }
            return n;
        }
        case DrillCue::Deny:
            return force_deny(row.tag, now_us, buf, cap);
        case DrillCue::HangEnter:
        case DrillCue::HangPull:
            return hang(row.tag, 1) ? 0 : -1;
        case DrillCue::DropFill: {
            const int ti = find_tag(row.tag);
            if (ti < 0) return -1;
            Ticket& t = tickets_[ti];
            int64_t want = row.qty > 0 ? row.qty : (t.qty - t.done);
            return fill_silent(ti, want);
        }
        case DrillCue::Closed:
            cfg_.trade_open = 0;
            return 0;
        default:
            return -1;
    }
}

int PaperVenue::on_print(uint32_t name, int64_t px, int64_t qty, int64_t now_us, Report* buf, int cap) noexcept {
    return on_tape(name, px, qty, 0, now_us, buf, cap);
}

int PaperVenue::on_tape(uint32_t name, int64_t px, int64_t qty, uint8_t aggressor, int64_t now_us, Report* buf,
                        int cap) noexcept {
    if (name >= kNames || px <= 0 || qty <= 0) return 0;
    names_[name].last_px = px;
    int n = 0;
    int64_t print_left = qty;
    for (int i = name_head_[name]; i >= 0 && print_left > 0;) {
        if (buf && n >= cap) break;
        const int next = name_next_[i];
        Ticket& t = tickets_[i];
        if (!tape_keep(aggressor, t.side, cfg_.rigid != 0)) {
            i = next;
            continue;
        }
        const uint8_t phase = t.phase;
        if (phase != static_cast<uint8_t>(Phase::Working) && phase != static_cast<uint8_t>(Phase::Partial) &&
            phase != static_cast<uint8_t>(Phase::PullWait)) {
            i = next;
            continue;
        }
        const int64_t extra = cfg_.rigid_extra;
        const bool ok = buy_side(t.side) ? px <= t.px - extra : px >= t.px + extra;
        if (!ok) {
            i = next;
            continue;
        }
        const int64_t leaves = t.qty - t.done;
        int64_t got = 0;
        int64_t used = 0;
        if (buy_side(t.side) && px < t.px) {
            got = leaves < print_left ? leaves : print_left;
            used = got;
        } else if (sell_side(t.side) && px > t.px) {
            got = leaves < print_left ? leaves : print_left;
            used = got;
        } else {
            got = gap_print_used(t.gap, print_left, t.lot, leaves, &used);
        }
        if (used > print_left) used = print_left;
        if (used < 0) used = 0;
        print_left -= used;
        if (got <= 0) {
            i = next;
            continue;
        }
        n = fill_ticket(i, px, got, now_us, buf, cap, n);
        if (t.used && t.done >= t.qty) free_ticket(i);
        i = next;
    }
    return n;
}

int PaperVenue::on_ext(uint32_t name, const ExtPrint* prints, int nprints, int64_t now_us, Report* buf, int cap) noexcept {
    if (name >= kNames || !prints || nprints <= 0) return 0;
    if (name_head_[name] < 0) {
        for (int p = 0; p < nprints; ++p) {
            if (prints[p].px > 0) names_[name].last_px = prints[p].px;
        }
        return 0;
    }
    int n = 0;
    for (int p = 0; p < nprints; ++p) {
        ExtPrint print = prints[p];
        if (print.px > 0) names_[name].last_px = print.px;
        if (print.qty <= 0) continue;
        for (int i = name_head_[name]; i >= 0 && print.qty > 0;) {
            if (buf && n >= cap) break;
            const int next = name_next_[i];
            Ticket& t = tickets_[i];
            const uint8_t phase = t.phase;
            if (phase != static_cast<uint8_t>(Phase::Working) && phase != static_cast<uint8_t>(Phase::Partial) &&
                phase != static_cast<uint8_t>(Phase::PullWait)) {
                i = next;
                continue;
            }
            const int64_t leaves = t.qty - t.done;
            if (leaves <= 0 || t.born_us < 0) {
                i = next;
                continue;
            }
            const PathFill hit = ext_match_one(buy_side(t.side), t.px, leaves, cfg_.rigid_extra, cfg_.rigid != 0,
                                               static_cast<uint64_t>(t.born_us), print);
            if (hit.qty <= 0) {
                i = next;
                continue;
            }
            n = fill_ticket(i, hit.px, hit.qty, now_us, buf, cap, n);
            print.qty -= hit.qty;
            if (t.used && t.done >= t.qty) free_ticket(i);
            i = next;
        }
    }
    return n;
}

int PaperVenue::on_shown(uint32_t name, uint8_t side, int64_t px, int64_t prev_qty, int64_t now_qty, int64_t now_us,
                         Report* buf, int cap) noexcept {
    if (name >= kNames || !known_side(side)) return 0;
    NameState& ns = names_[name];
    Rung* row = buy_side(side) ? ns.bids : ns.asks;
    int* nn = buy_side(side) ? &ns.nbid : &ns.nask;
    bool found = false;
    for (int i = 0; i < *nn; ++i) {
        if (row[i].px != px) continue;
        found = true;
        row[i].qty = now_qty;
        if (now_qty <= 0) {
            for (int j = i; j < *nn - 1; ++j) row[j] = row[j + 1];
            --(*nn);
        }
        break;
    }
    if (!found && now_qty > 0 && *nn < static_cast<int>(kDepth)) {
        int at = 0;
        while (at < *nn && (buy_side(side) ? row[at].px > px : row[at].px < px)) ++at;
        for (int j = *nn; j > at; --j) row[j] = row[j - 1];
        row[at] = Rung{px, now_qty};
        ++(*nn);
    }
    if (*nn > 0) {
        if (buy_side(side)) {
            ns.bid_px = row[0].px;
            ns.bid_qty = row[0].qty;
        } else {
            ns.ask_px = row[0].px;
            ns.ask_qty = row[0].qty;
        }
    } else if (buy_side(side)) {
        ns.bid_px = 0;
        ns.bid_qty = 0;
    } else {
        ns.ask_px = 0;
        ns.ask_qty = 0;
    }
    int n = 0;
    if (name_head_[name] < 0) return n;
    for (int i = name_head_[name]; i >= 0;) {
        if (buf && n >= cap) break;
        const int next = name_next_[i];
        Ticket& t = tickets_[i];
        if (t.side != side || t.px != px) {
            i = next;
            continue;
        }
        const uint8_t phase = t.phase;
        if (phase != static_cast<uint8_t>(Phase::Working) && phase != static_cast<uint8_t>(Phase::Partial) &&
            phase != static_cast<uint8_t>(Phase::PullWait)) {
            i = next;
            continue;
        }
        const int64_t leaves = t.qty - t.done;
        const int64_t got = gap_resize(t.gap, prev_qty, now_qty, cfg_.cancel_pow, t.lot, leaves);
        if (got <= 0) {
            i = next;
            continue;
        }
        n = fill_ticket(i, px, got, now_us, buf, cap, n);
        if (t.used && t.done >= t.qty) free_ticket(i);
        i = next;
    }
    return n;
}

}  // namespace pitlane
