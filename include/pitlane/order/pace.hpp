#pragma once

#include "pitlane/desk/codec.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/order/ledger.hpp"
#include "pitlane/order/parent.hpp"
#include "pitlane/order/quoteprice.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Execution pacing. These objects own the decision loop but never send: every order they
// emit is a plain record the caller books and routes. All state is a fixed array; nothing
// allocates. A terminal state is sticky, and every rejected transition leaves the object
// unchanged.

enum class PaceState : uint8_t {
    Idle = 0,
    Slicing = 1,
    Waiting = 2,
    Paused = 3,
    Done = 4,
    Stopped = 5,
};

struct PaceOrder {
    uint64_t id{0};
    int64_t qty{0};
    int64_t px{0};
    uint8_t side{0};
    uint8_t off{0};
    uint8_t last{0};
};

struct PaceCtl {
    PaceState state{PaceState::Idle};

    bool start() noexcept {
        if (state != PaceState::Idle) return false;
        state = PaceState::Slicing;
        return true;
    }
    bool pause() noexcept {
        if (state != PaceState::Slicing && state != PaceState::Waiting) return false;
        state = PaceState::Paused;
        return true;
    }
    bool resume() noexcept {
        if (state != PaceState::Paused) return false;
        state = PaceState::Slicing;
        return true;
    }
    bool stop() noexcept {
        if (terminal()) return false;
        state = PaceState::Stopped;
        return true;
    }
    bool finish() noexcept {
        if (terminal()) return false;
        state = PaceState::Done;
        return true;
    }
    bool terminal() const noexcept { return state == PaceState::Done || state == PaceState::Stopped; }
    bool running() const noexcept { return state == PaceState::Slicing || state == PaceState::Waiting; }
    bool paused() const noexcept { return state == PaceState::Paused; }
    void reset() noexcept { state = PaceState::Idle; }
};

namespace pace_detail {

PITLANE_INLINE int64_t lot_floor(int64_t qty, int64_t lot) noexcept {
    if (qty <= 0 || lot <= 0) return 0;
    if (lot == 1 || qty < lot) return qty < lot ? 0 : qty;
    return (qty / lot) * lot;
}

PITLANE_INLINE bool would_cross(bool buy, int64_t px, const QuoteTop& q) noexcept {
    if (q.bid[0] <= 0 || q.ask[0] <= 0) return false;
    if (buy) return px >= q.ask[0];
    return px <= q.bid[0];
}

PITLANE_INLINE bool buy_move(MoveKind kind) noexcept {
    return kind == MoveKind::BuyOpen || kind == MoveKind::BuyClose || kind == MoveKind::BuyCloseToday ||
           kind == MoveKind::BuyCloseYday;
}

PITLANE_INLINE uint8_t off_for(MoveKind kind) noexcept {
    switch (kind) {
        case MoveKind::BuyOpen:
        case MoveKind::SellOpen:
            return static_cast<uint8_t>(Off::Open);
        case MoveKind::BuyCloseToday:
        case MoveKind::SellCloseToday:
            return static_cast<uint8_t>(Off::CloseToday);
        case MoveKind::BuyCloseYday:
        case MoveKind::SellCloseYday:
            return static_cast<uint8_t>(Off::CloseYday);
        case MoveKind::BuyClose:
        case MoveKind::SellClose:
            return static_cast<uint8_t>(Off::Close);
        case MoveKind::None:
            break;
    }
    return static_cast<uint8_t>(Off::Open);
}

}  // namespace pace_detail

struct SliceCfg {
    int64_t total{0};
    int64_t horizon_us{0};
    int64_t interval_us{0};
    int64_t lot{1};
    int64_t t0_us{0};
    int64_t tick{0};
    int64_t post_steps{0};
    uint8_t side{static_cast<uint8_t>(Side::Buy)};
    uint8_t mode{0};
    uint8_t off{static_cast<uint8_t>(Off::Open)};
};

// Time-sliced parent. One child per poll; the next cut is due from the plan's own clock.
// A cut that cannot be priced is returned to the pool and the remaining cuts still run.
// After the last cut, one catch-up child carries whatever the pool still holds.
template <int Children = 64>
class SlicePacer {
public:
    static_assert(Children >= 1 && Children <= 256, "pace children");

    bool open(uint64_t id, const SliceCfg& cfg) noexcept {
        if (ctl_.running() || ctl_.paused()) return false;
        ctl_.reset();
        n_ = 0;
        working_ = 0;
        last_us_ = 0;
        end_done_ = 0;
        next_child_ = 1;
        for (int i = 0; i < Children; ++i) child_[i] = ChildRef{};
        if (id == 0 || !known_side(cfg.side) || cfg.tick < 0 || cfg.mode > 4 || cfg.lot <= 0) return false;
        if (static_cast<uint8_t>(cfg.off) > static_cast<uint8_t>(Off::CloseYday)) return false;
        if (!slice_open(plan_, cfg.total, cfg.horizon_us, cfg.interval_us, cfg.lot, cfg.t0_us)) return false;
        id_ = id;
        cfg_ = cfg;
        ctl_.start();
        return true;
    }

    PITLANE_HOT int poll(int64_t now_us, const QuoteTop& q, PaceOrder* out, int cap) noexcept {
        if (!ctl_.running() || !out || cap <= 0) return 0;
        if (n_ >= Children) return 0;
        int64_t qty = 0;
        uint8_t last = 0;
        uint8_t catchup = 0;
        SliceCut cut{};
        if (slice_poll(plan_, now_us, cut)) {
            qty = cut.qty;
            last = cut.last;
        } else if (end_done_ == 0 && plan_.sent >= plan_.cuts && plan_.pool > 0) {
            qty = plan_.pool;
            last = 1;
            catchup = 1;
            plan_.pool = 0;
            plan_.scheduled += qty;
        } else {
            return 0;
        }
        if (qty <= 0) return 0;
        const int64_t keep = pace_detail::lot_floor(qty, cfg_.lot);
        if (keep <= 0) {
            (void)slice_restore(plan_, qty);
            return 0;
        }
        if (keep < qty && !slice_restore(plan_, qty - keep)) return 0;
        const bool buy = cfg_.side == static_cast<uint8_t>(Side::Buy);
        int64_t px = 0;
        if (cfg_.post_steps > 0) px = post_px(buy, q.bid[0], q.ask[0], cfg_.tick, cfg_.post_steps);
        else px = pre_px(buy, cfg_.mode, q, false, false);
        if (px <= 0) {
            (void)slice_restore(plan_, keep);
            return 0;
        }
        ChildRef& c = child_[n_++];
        c = ChildRef{};
        c.id = next_child_++;
        if (next_child_ == 0) next_child_ = 1;
        c.qty = keep;
        c.px = px;
        c.live = 1;
        out[0] = PaceOrder{c.id, keep, px, cfg_.side, cfg_.off, last};
        working_ += keep;
        last_us_ = now_us;
        if (catchup != 0) end_done_ = 1;
        ctl_.state = PaceState::Waiting;
        return 1;
    }

    bool on_child(uint64_t child, int64_t qty, bool done) noexcept {
        const int i = find_child(child);
        if (i < 0 || qty < 0) return false;
        ChildRef& c = child_[i];
        int64_t room = c.qty - c.done_qty;
        int64_t fill = qty < room ? qty : room;
        int64_t release = 0;
        if (done && room - fill > 0) release = room - fill;
        if (release > 0 && release > plan_.scheduled) return false;
        if (fill > 0 && !slice_fill(plan_, fill)) return false;
        if (fill > 0) {
            c.done_qty += fill;
            working_ -= fill;
        }
        if (done) {
            if (release > 0) {
                if (!slice_release(plan_, release)) return false;
                working_ -= release;
            }
            c.live = 0;
            if (working_ < 0) working_ = 0;
            if (!ctl_.terminal()) ctl_.state = any_live() ? PaceState::Waiting : PaceState::Slicing;
        }
        return true;
    }

    bool on_reject(uint64_t child) noexcept {
        const int i = find_child(child);
        if (i < 0) return false;
        ChildRef& c = child_[i];
        const int64_t rem = c.qty - c.done_qty;
        if (rem > plan_.scheduled) return false;
        if (rem > 0 && !slice_release(plan_, rem)) return false;
        working_ -= rem;
        if (working_ < 0) working_ = 0;
        c.live = 0;
        if (!ctl_.terminal()) ctl_.state = any_live() ? PaceState::Waiting : PaceState::Slicing;
        return true;
    }

    bool pause() noexcept { return ctl_.pause(); }
    bool resume() noexcept { return ctl_.resume(); }

    bool stop() noexcept {
        if (!ctl_.stop()) return false;
        for (int i = 0; i < n_; ++i) {
            if (child_[i].live) child_[i].live = 2;  // still reported to the caller for cancel
        }
        return true;
    }

    bool close() noexcept {
        if (plan_.pool != 0 || plan_.scheduled != 0) return false;
        if (ctl_.state == PaceState::Done) return true;
        if (!ctl_.running() && !ctl_.paused()) return false;
        return ctl_.finish();
    }

    bool progress(int64_t& done, int64_t& working) const noexcept {
        done = plan_.filled;
        working = working_;
        return true;
    }

    int live_children(PaceOrder* out, int cap) const noexcept {
        int n = 0;
        for (int i = 0; i < n_; ++i) {
            if (!child_[i].live) continue;
            if (!out || n >= cap) return -1;
            out[n].id = child_[i].id;
            out[n].qty = child_[i].qty - child_[i].done_qty;
            out[n].px = child_[i].px;
            out[n].side = cfg_.side;
            out[n].off = cfg_.off;
            out[n].last = 0;
            ++n;
        }
        return n;
    }

    uint64_t id() const noexcept { return id_; }
    PaceState state() const noexcept { return ctl_.state; }
    bool terminal() const noexcept { return ctl_.terminal(); }
    bool running() const noexcept { return ctl_.running(); }
    int children() const noexcept { return n_; }
    int64_t last_send_us() const noexcept { return last_us_; }

private:
    struct ChildRef {
        uint64_t id{0};
        int64_t qty{0};
        int64_t px{0};
        int64_t done_qty{0};
        uint8_t live{0};
    };

    int find_child(uint64_t id) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (child_[i].id == id && child_[i].live) return i;
        }
        return -1;
    }

    bool any_live() const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (child_[i].live == 1) return true;
        }
        return false;
    }

    uint64_t id_{0};
    SliceCfg cfg_{};
    SlicePlan plan_{};
    PaceCtl ctl_{};
    ChildRef child_[Children]{};
    int n_{0};
    uint64_t next_child_{1};
    int64_t working_{0};
    int64_t last_us_{0};
    uint8_t end_done_{0};
};

struct ChaseCfg {
    int64_t max_lot{0};
    int64_t min_lot{1};
    int64_t tick{0};
    int64_t chase_steps{1};
    int64_t post_steps{1};
    int64_t retry_us{0};
    uint8_t cross_ok{0};
    uint8_t plan_kind{0};  // 0 two-leg, 1 net index
};

// Position release with two-sided posting. Each tick asks the book plan for the next move,
// prices it from the touch, refuses a cross unless allowed, and emits one child. A child
// that outlives the retry window is reported through stale_children so the caller cancels it.
template <int Children = 64>
class ChasePacer {
public:
    static_assert(Children >= 1 && Children <= 256, "pace children");

    bool open(uint64_t id, const ChaseBook& book, const ChaseCfg& cfg) noexcept {
        if (ctl_.running() || ctl_.paused()) return false;
        ctl_.reset();
        n_ = 0;
        working_ = 0;
        last_us_ = 0;
        sent_any_ = 0;
        next_child_ = 1;
        for (int i = 0; i < Children; ++i) child_[i] = ChildRef{};
        if (id == 0 || cfg.min_lot <= 0 || cfg.max_lot < 0 || cfg.tick <= 0) return false;
        if (cfg.chase_steps < 0 || cfg.post_steps < 0 || cfg.retry_us < 0 || cfg.cross_ok > 1) return false;
        if (cfg.plan_kind > 1) return false;
        id_ = id;
        book_ = book;
        cfg_ = cfg;
        ctl_.start();
        return true;
    }

    PITLANE_HOT int tick(int64_t now_us, const QuoteTop& q, PaceOrder* out, int cap) noexcept {
        if (!ctl_.running() || !out || cap <= 0 || n_ >= Children) return 0;
        if (cfg_.retry_us > 0 && sent_any_ != 0 && now_us >= last_us_) {
            if (now_us - last_us_ < cfg_.retry_us) return 0;
        }
        const Move move = cfg_.plan_kind == 0 ? commodity_plan(book_) : index_plan(book_);
        if (move.kind == MoveKind::None) {
            if (!any_live() && !ctl_.terminal()) {
                if (at_target()) (void)ctl_.finish();
                else ctl_.state = PaceState::Slicing;
            }
            return 0;
        }
        const bool buy = pace_detail::buy_move(move.kind);
        int64_t px = 0;
        if (cfg_.chase_steps > 0) px = chasing_px(buy, 0, q, cfg_.tick, cfg_.chase_steps);
        else px = post_px(buy, q.bid[0], q.ask[0], cfg_.tick, cfg_.post_steps);
        if (px <= 0) return 0;
        if (!cfg_.cross_ok && pace_detail::would_cross(buy, px, q)) {
            px = post_px(buy, q.bid[0], q.ask[0], cfg_.tick, 1);
            if (px <= 0 || pace_detail::would_cross(buy, px, q)) return 0;
        }
        int64_t qty = move.qty;
        if (cfg_.max_lot > 0 && qty > cfg_.max_lot) qty = cfg_.max_lot;
        qty = pace_detail::lot_floor(qty, cfg_.min_lot);
        if (qty <= 0) return 0;
        ChildRef& c = child_[n_++];
        c = ChildRef{};
        c.id = next_child_++;
        if (next_child_ == 0) next_child_ = 1;
        c.qty = qty;
        c.px = px;
        c.kind = move.kind;
        c.live = 1;
        c.sent_us = now_us;
        out[0] = PaceOrder{c.id, qty, px, buy ? static_cast<uint8_t>(Side::Buy) : static_cast<uint8_t>(Side::Sell),
                           pace_detail::off_for(move.kind), 0};
        working_ += qty;
        last_us_ = now_us;
        sent_any_ = 1;
        ctl_.state = PaceState::Waiting;
        return 1;
    }

    bool on_child(uint64_t child, int64_t qty, bool done) noexcept {
        const int i = find_child(child);
        if (i < 0 || qty < 0) return false;
        ChildRef& c = child_[i];
        const int64_t room = c.qty - c.done_qty;
        const int64_t fill = qty < room ? qty : room;
        if (fill > 0) {
            book_fill(book_, c.kind, fill);
            c.done_qty += fill;
            working_ -= fill;
        }
        if (done) {
            c.live = 0;
            if (working_ < 0) working_ = 0;
            if (!ctl_.terminal()) ctl_.state = any_live() ? PaceState::Waiting : PaceState::Slicing;
        }
        return true;
    }

    bool on_reject(uint64_t child) noexcept {
        const int i = find_child(child);
        if (i < 0) return false;
        ChildRef& c = child_[i];
        working_ -= c.qty - c.done_qty;
        if (working_ < 0) working_ = 0;
        c.live = 0;
        if (!ctl_.terminal()) ctl_.state = any_live() ? PaceState::Waiting : PaceState::Slicing;
        return true;
    }

    int stale_children(int64_t now_us, PaceOrder* out, int cap) const noexcept {
        if (cfg_.retry_us <= 0) return 0;
        int n = 0;
        for (int i = 0; i < n_; ++i) {
            const ChildRef& c = child_[i];
            if (!c.live || now_us < c.sent_us) continue;
            if (now_us - c.sent_us < cfg_.retry_us) continue;
            if (!out || n >= cap) return -1;
            out[n].id = c.id;
            out[n].qty = c.qty - c.done_qty;
            out[n].px = c.px;
            out[n].side = pace_detail::buy_move(c.kind) ? static_cast<uint8_t>(Side::Buy)
                                                        : static_cast<uint8_t>(Side::Sell);
            out[n].off = pace_detail::off_for(c.kind);
            out[n].last = 0;
            ++n;
        }
        return n;
    }

    bool pause() noexcept { return ctl_.pause(); }
    bool resume() noexcept { return ctl_.resume(); }

    bool stop() noexcept {
        if (!ctl_.stop()) return false;
        for (int i = 0; i < n_; ++i) {
            if (child_[i].live) child_[i].live = 2;
        }
        return true;
    }

    bool progress(int64_t& done, int64_t& working) const noexcept {
        done = book_.lng.closed + book_.sht.closed;
        working = working_;
        return true;
    }

    int64_t net() const noexcept { return book_.lng.pos - book_.sht.pos; }
    bool at_target() const noexcept { return net() == book_.target; }
    uint64_t id() const noexcept { return id_; }
    PaceState state() const noexcept { return ctl_.state; }
    bool terminal() const noexcept { return ctl_.terminal(); }
    bool running() const noexcept { return ctl_.running(); }
    const ChaseBook& book() const noexcept { return book_; }

private:
    struct ChildRef {
        uint64_t id{0};
        int64_t qty{0};
        int64_t px{0};
        int64_t done_qty{0};
        int64_t sent_us{0};
        MoveKind kind{MoveKind::None};
        uint8_t live{0};
    };

    int find_child(uint64_t id) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (child_[i].id == id && child_[i].live) return i;
        }
        return -1;
    }

    bool any_live() const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (child_[i].live == 1) return true;
        }
        return false;
    }

    uint64_t id_{0};
    ChaseBook book_{};
    ChaseCfg cfg_{};
    PaceCtl ctl_{};
    ChildRef child_[Children]{};
    int n_{0};
    uint64_t next_child_{1};
    int64_t working_{0};
    int64_t last_us_{0};
    uint8_t sent_any_{0};
};

struct SizeCfg {
    int64_t total{0};
    int64_t threshold{0};
    int64_t lot{1};
    int64_t max_child{0};
    int64_t tick{0};
    uint8_t side{static_cast<uint8_t>(Side::Buy)};
    uint8_t mode{0};  // 0 join own top, 1 take opposite top, 2 opposite depth
    uint8_t off{static_cast<uint8_t>(Off::Open)};
};

// Size-threshold child sizing. When the opposite touch shows at least the threshold, the
// child takes that size; otherwise it joins the own touch or takes the opposite top,
// depending on the mode. Every child is clipped by max_child and the lot.
template <int Children = 32>
class SizePacer {
public:
    static_assert(Children >= 1 && Children <= 256, "pace children");

    bool open(uint64_t id, const SizeCfg& cfg) noexcept {
        if (ctl_.running() || ctl_.paused()) return false;
        ctl_.reset();
        n_ = 0;
        working_ = 0;
        done_ = 0;
        next_child_ = 1;
        for (int i = 0; i < Children; ++i) child_[i] = ChildRef{};
        if (id == 0 || cfg.total <= 0 || cfg.threshold < 0 || cfg.lot <= 0 || cfg.max_child < 0 || cfg.tick < 0)
            return false;
        if (!known_side(cfg.side) || cfg.mode > 2) return false;
        if (static_cast<uint8_t>(cfg.off) > static_cast<uint8_t>(Off::CloseYday)) return false;
        id_ = id;
        cfg_ = cfg;
        ctl_.start();
        return true;
    }

    PITLANE_HOT int poll(const QuoteTop& q, PaceOrder* out, int cap) noexcept {
        if (!ctl_.running() || !out || cap <= 0 || n_ >= Children) return 0;
        const int64_t left = cfg_.total - done_ - working_;
        if (left <= 0) {
            if (!any_live()) (void)ctl_.finish();
            return 0;
        }
        const bool buy = cfg_.side == static_cast<uint8_t>(Side::Buy);
        const int64_t opp_px = buy ? q.ask[0] : q.bid[0];
        const int64_t opp_qty = buy ? q.ask_qty : q.bid_qty;
        const bool big = cfg_.threshold > 0 && opp_qty >= cfg_.threshold && opp_px > 0;
        int64_t px = 0;
        int64_t qty = left;
        if (big) {
            px = opp_px;
            if (qty > opp_qty) qty = opp_qty;
        } else if (cfg_.mode == 0) {
            px = buy ? q.bid[0] : q.ask[0];
        } else if (cfg_.mode == 1) {
            px = opp_px;
        } else {
            px = pre_px(buy, 0, q, false, false);
            if (px <= 0) px = opp_px;
        }
        if (px <= 0) return 0;
        if (cfg_.max_child > 0 && qty > cfg_.max_child) qty = cfg_.max_child;
        qty = pace_detail::lot_floor(qty, cfg_.lot);
        if (qty <= 0) return 0;
        ChildRef& c = child_[n_++];
        c = ChildRef{};
        c.id = next_child_++;
        if (next_child_ == 0) next_child_ = 1;
        c.qty = qty;
        c.px = px;
        c.live = 1;
        out[0] = PaceOrder{c.id, qty, px, cfg_.side, cfg_.off, 0};
        working_ += qty;
        ctl_.state = PaceState::Waiting;
        return 1;
    }

    bool on_child(uint64_t child, int64_t qty, bool done) noexcept {
        const int i = find_child(child);
        if (i < 0 || qty < 0) return false;
        ChildRef& c = child_[i];
        const int64_t room = c.qty - c.done_qty;
        const int64_t fill = qty < room ? qty : room;
        c.done_qty += fill;
        done_ += fill;
        working_ -= fill;
        if (done) {
            working_ -= c.qty - c.done_qty;
            if (working_ < 0) working_ = 0;
            c.live = 0;
            if (!ctl_.terminal()) ctl_.state = any_live() ? PaceState::Waiting : PaceState::Slicing;
            if (done_ >= cfg_.total && !any_live()) (void)ctl_.finish();
        }
        return true;
    }

    bool on_reject(uint64_t child) noexcept {
        const int i = find_child(child);
        if (i < 0) return false;
        working_ -= child_[i].qty - child_[i].done_qty;
        if (working_ < 0) working_ = 0;
        child_[i].live = 0;
        if (!ctl_.terminal()) ctl_.state = any_live() ? PaceState::Waiting : PaceState::Slicing;
        return true;
    }

    bool pause() noexcept { return ctl_.pause(); }
    bool resume() noexcept { return ctl_.resume(); }

    bool stop() noexcept {
        if (!ctl_.stop()) return false;
        for (int i = 0; i < n_; ++i) {
            if (child_[i].live) child_[i].live = 2;
        }
        return true;
    }

    bool progress(int64_t& done, int64_t& working) const noexcept {
        done = done_;
        working = working_;
        return true;
    }

    uint64_t id() const noexcept { return id_; }
    PaceState state() const noexcept { return ctl_.state; }
    bool terminal() const noexcept { return ctl_.terminal(); }
    bool running() const noexcept { return ctl_.running(); }

private:
    struct ChildRef {
        uint64_t id{0};
        int64_t qty{0};
        int64_t px{0};
        int64_t done_qty{0};
        uint8_t live{0};
    };

    int find_child(uint64_t id) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (child_[i].id == id && child_[i].live) return i;
        }
        return -1;
    }

    bool any_live() const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (child_[i].live == 1) return true;
        }
        return false;
    }

    uint64_t id_{0};
    SizeCfg cfg_{};
    PaceCtl ctl_{};
    ChildRef child_[Children]{};
    int n_{0};
    uint64_t next_child_{1};
    int64_t working_{0};
    int64_t done_{0};
};

// Fixed-capacity registry. A duplicate id or a full table refuses the add and leaves every
// existing entry unchanged. tick_all polls each entry in registration order and stops when
// the caller's output is full.
template <int Cap = 16, int Children = 64>
class PaceBook {
public:
    static_assert(Cap >= 1 && Cap <= 64, "pace table");
    static_assert(Children >= 1 && Children <= 256, "pace children");

    enum class Kind : uint8_t { None = 0, Slice = 1, Chase = 2, Size = 3 };

    SlicePacer<Children>* add_slice(uint64_t id, const SliceCfg& cfg) noexcept {
        const int i = reserve(id, Kind::Slice);
        if (i < 0) return nullptr;
        if (!slice_[i].open(id, cfg)) {
            used_[i] = 0;
            return nullptr;
        }
        return &slice_[i];
    }

    ChasePacer<Children>* add_chase(uint64_t id, const ChaseBook& book, const ChaseCfg& cfg) noexcept {
        const int i = reserve(id, Kind::Chase);
        if (i < 0) return nullptr;
        if (!chase_[i].open(id, book, cfg)) {
            used_[i] = 0;
            return nullptr;
        }
        return &chase_[i];
    }

    SizePacer<Children>* add_size(uint64_t id, const SizeCfg& cfg) noexcept {
        const int i = reserve(id, Kind::Size);
        if (i < 0) return nullptr;
        if (!size_[i].open(id, cfg)) {
            used_[i] = 0;
            return nullptr;
        }
        return &size_[i];
    }

    int find(uint64_t id) const noexcept {
        if (id == 0) return -1;
        for (int i = 0; i < Cap; ++i) {
            if (used_[i] && id_[i] == id) return i;
        }
        return -1;
    }

    Kind kind(uint64_t id) const noexcept {
        const int i = find(id);
        return i < 0 ? Kind::None : kind_[i];
    }

    SlicePacer<Children>* slice(uint64_t id) noexcept {
        const int i = find(id);
        return i >= 0 && kind_[i] == Kind::Slice ? &slice_[i] : nullptr;
    }

    ChasePacer<Children>* chase(uint64_t id) noexcept {
        const int i = find(id);
        return i >= 0 && kind_[i] == Kind::Chase ? &chase_[i] : nullptr;
    }

    SizePacer<Children>* size(uint64_t id) noexcept {
        const int i = find(id);
        return i >= 0 && kind_[i] == Kind::Size ? &size_[i] : nullptr;
    }

    bool pause(uint64_t id) noexcept {
        const int i = find(id);
        if (i < 0) return false;
        if (kind_[i] == Kind::Slice) return slice_[i].pause();
        if (kind_[i] == Kind::Chase) return chase_[i].pause();
        return size_[i].pause();
    }

    bool resume(uint64_t id) noexcept {
        const int i = find(id);
        if (i < 0) return false;
        if (kind_[i] == Kind::Slice) return slice_[i].resume();
        if (kind_[i] == Kind::Chase) return chase_[i].resume();
        return size_[i].resume();
    }

    bool stop(uint64_t id) noexcept {
        const int i = find(id);
        if (i < 0) return false;
        if (kind_[i] == Kind::Slice) return slice_[i].stop();
        if (kind_[i] == Kind::Chase) return chase_[i].stop();
        return size_[i].stop();
    }

    bool retire(uint64_t id) noexcept {
        const int i = find(id);
        if (i < 0) return false;
        bool over = false;
        if (kind_[i] == Kind::Slice) over = slice_[i].terminal();
        else if (kind_[i] == Kind::Chase) over = chase_[i].terminal();
        else over = size_[i].terminal();
        if (!over) return false;
        used_[i] = 0;
        id_[i] = 0;
        kind_[i] = Kind::None;
        return true;
    }

    int tick_all(int64_t now_us, const QuoteTop& q, PaceOrder* out, int cap) noexcept {
        if (!out || cap <= 0) return 0;
        int n = 0;
        for (int i = 0; i < Cap && n < cap; ++i) {
            if (!used_[i]) continue;
            int got = 0;
            if (kind_[i] == Kind::Slice) got = slice_[i].poll(now_us, q, out + n, cap - n);
            else if (kind_[i] == Kind::Chase) got = chase_[i].tick(now_us, q, out + n, cap - n);
            else got = size_[i].poll(q, out + n, cap - n);
            if (got > 0) n += got;
        }
        return n;
    }

    int count() const noexcept {
        int n = 0;
        for (int i = 0; i < Cap; ++i) {
            if (used_[i]) ++n;
        }
        return n;
    }

    static constexpr int capacity() noexcept { return Cap; }

private:
    int reserve(uint64_t id, Kind kind) noexcept {
        if (id == 0 || find(id) >= 0) return -1;
        for (int i = 0; i < Cap; ++i) {
            if (used_[i]) continue;
            used_[i] = 1;
            id_[i] = id;
            kind_[i] = kind;
            return i;
        }
        return -1;
    }

    uint64_t id_[Cap]{};
    Kind kind_[Cap]{};
    uint8_t used_[Cap]{};
    SlicePacer<Children> slice_[Cap]{};
    ChasePacer<Children> chase_[Cap]{};
    SizePacer<Children> size_[Cap]{};
};

}  // namespace pitlane
