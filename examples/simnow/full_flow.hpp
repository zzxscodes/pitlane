// Full-flow validation harness for the SimNow example.
// MD thread feeds factors and market state; the main thread owns OMS/PMS/EMS,
// routing, risk, WAL, snapshots, and tail telemetry.
//
// Covered in one run: quotes -> factors -> signal -> RMS -> parent/EMS slices ->
// SOR -> OMS state machine -> live CTP or paper venue -> PMS -> WAL -> snapshot ->
// tail latency. Every path is fixed-capacity and allocates nothing after open().

#pragma once

#include "pitlane/desk/ids.hpp"
#include "pitlane/desk/region.hpp"
#include "pitlane/desk/session.hpp"
#include "pitlane/desk/snapring.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/desk/wal.hpp"
#include "pitlane/order/ems.hpp"
#include "pitlane/order/oms.hpp"
#include "pitlane/order/pms.hpp"
#include "pitlane/order/rms.hpp"
#include "pitlane/ops/control.hpp"
#include "pitlane/study/factor.hpp"
#include "pitlane/study/factor_eval.hpp"
#include "pitlane/study/perf.hpp"
#include "pitlane/study/pipeline.hpp"
#include "pitlane/time/cycle.hpp"
#include "pitlane/time/mag.hpp"
#include "pitlane/time/tail.hpp"
#include "pitlane/util/spin.hpp"
#include "pitlane/util/wirelog.hpp"
#include "pitlane/version.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace pitlane {

struct FlowSnap {
    uint64_t seq;
    int64_t net_qty;
    int64_t realized;
    int64_t last_px;
    int64_t momentum_ppm;
    int64_t ofi_ppm;
    int32_t orders;
    int32_t fills;
    int32_t parents_closed;
    int32_t payload_orders;
};

struct FlowNote {
    uint64_t tag;
    int64_t px;
    int64_t qty;
    uint32_t name;
    uint8_t kind;
    uint8_t pad[3];
};

struct FlowCfg {
    uint8_t kind{static_cast<uint8_t>(FactorKind::Momentum)};
    uint16_t window{16};
    int8_t sign{1};
    int64_t signal_ppm{10};
};

// Read-only views for the ops console and the risk guard.
struct FlowCounters {
    int64_t quotes{0};
    int64_t submitted{0};
    int64_t acked{0};
    int64_t fills{0};
    int64_t rejects{0};
    int64_t cancels{0};
    int64_t rms_reject{0};
    int64_t daily_notional{0};
    int64_t last_px{0};
    int64_t last_tick_us{0};
    int64_t realized{0};
    int64_t unrealized{0};
    int64_t fees{0};
    int64_t parents_closed{0};
    int32_t live_orders{0};
    int32_t rate_1s{0};
};

struct FlowPos {
    uint32_t name{0};
    int64_t long_qty{0};
    int64_t short_qty{0};
    int64_t long_yday{0};
    int64_t short_yday{0};
    int64_t avg_long_px{0};
    int64_t avg_short_px{0};
    int64_t realized{0};
    int64_t unrealized{0};
};

struct FlowOrderRow {
    uint64_t tag{0};
    uint32_t name{0};
    uint8_t side{0};
    uint8_t phase{0};
    int64_t qty{0};
    int64_t filled{0};
    int64_t px{0};
    int64_t updated_us{0};
};

class FullFlow {
public:
    static constexpr int kParentQty = 3;
    static constexpr int kChildCap = 64;
    static constexpr int kEventCap = 256;
    static constexpr int kRecentOrders = 32;
    static constexpr int kMaxNames = 8;

    using SendFn = int (*)(void* user, const Intent& in);
    // Ops beat hook, run on the trading thread ahead of the strategy.
    using OpsBeatFn = int (*)(void* user, int64_t now_us, FullFlow& flow);

    bool open(const Account& acct, int mode, WireLog* log, const FlowCfg& cfg = FlowCfg{}) {
        if (open_) return false;
        log_ = log;
        mode_ = mode;
        switch (mode) {
            case 1:
                stage_ = PipeStage::Live;
                break;
            case 2:
            case 3:
                stage_ = PipeStage::Research;
                break;
            default:
                return false;
        }
        std::snprintf(wal_path_, sizeof wal_path_, "%s/flow.wal", acct.work_dir.c_str());
        std::snprintf(snap_path_, sizeof snap_path_, "%s/flow.snap", acct.work_dir.c_str());

        RmsLimits lim{};
        lim.max_order_qty = 10;
        lim.max_order_notional = 200000000;
        lim.max_pos_long = mode == 1 ? 100 : 100000;
        lim.max_pos_short = mode == 1 ? 100 : 100000;
        lim.max_daily_notional = 2000000000;
        // Research/backtest replay compresses many slices into one second.
        lim.max_orders_per_sec = mode == 1 ? 5 : 64;
        lim.price_band_ppm = 100000;
        lim.max_live_orders = mode == 1 ? 4 : 16;
        rms_.set(lim);

        tail_arm(tail_, 5000000, (1u << static_cast<int>(TailLane::Quote)) |
                                     (1u << static_cast<int>(TailLane::Report)) |
                                     (1u << static_cast<int>(TailLane::Signal)));

        FactorSpec sig{};
        sig.kind = cfg.kind;
        sig.window = cfg.window;
        sig.scale = 1000000;
        if (!pipe_.add(sig)) return false;
        confirm_slot_ = -1;
        if (cfg.kind != static_cast<uint8_t>(FactorKind::Ofi)) {
            FactorSpec ofi{};
            ofi.kind = static_cast<uint8_t>(FactorKind::Ofi);
            ofi.window = 2;
            ofi.scale = 1000000;
            if (!pipe_.add(ofi)) return false;
            confirm_slot_ = 1;
        }
        if (!pipe_.set_stage(stage_)) return false;
        signal_ppm_ = cfg.signal_ppm;
        sign_ = cfg.sign;
        cfg_kind_ = cfg.kind;
        cfg_window_ = cfg.window;

        ::unlink(wal_path_);
        MapOpt opt{};
        opt.hugepage = 0;
        opt.lock = 0;
        opt.prefault = 1;
        if (wal_open(wal_path_, 4096, &wal_, 1, opt) != 0) return false;
        wal_open_ = true;
        (void)wal_recover(wal_, [](const WalRec&) {});

        ::unlink(snap_path_);
        if (snap_open<FlowSnap, 64>(snap_path_, &snap_map_, 1, opt) != 0) {
            close();
            return false;
        }
        snap_ = snap_view<FlowSnap, 64>(&snap_map_, 1);
        if (!snap_) {
            close();
            return false;
        }
        open_ = true;
        if (log_) log_->log(2, lit("[flow] open mode="), mode_, lit(" wal="), wal_path_, lit(" snap="), snap_path_);
        return true;
    }

    void close() {
        if (wal_open_) {
            (void)wal_sync(wal_);
            wal_close(&wal_);
            wal_open_ = false;
        }
        if (snap_map_.addr) map_release(&snap_map_);
        snap_ = nullptr;
        open_ = false;
    }

    void set_send(SendFn fn, void* user) noexcept {
        send_ = fn;
        send_user_ = user;
    }

    void set_timing(int64_t parent_gap_us, int64_t horizon_us, int64_t interval_us) noexcept {
        parent_gap_us_ = parent_gap_us;
        horizon_us_ = horizon_us;
        interval_us_ = interval_us;
    }

    void set_control(ControlPlane* c) noexcept { control_ = c; }

    void set_ops(OpsBeatFn fn, void* user) noexcept {
        ops_ = fn;
        ops_user_ = user;
    }

    // Read-only views used by the ops console.
    Rms& rms() noexcept { return rms_; }
    const Rms& rms() const noexcept { return rms_; }
    IdMint& mint() noexcept { return mint_; }

    void last_quote(uint32_t name, int64_t& bid, int64_t& ask) const noexcept {
        if (name != 1) {
            bid = 0;
            ask = 0;
            return;
        }
        bid = bid_px_.load(std::memory_order_relaxed);
        ask = ask_px_.load(std::memory_order_relaxed);
    }

    int64_t net_qty(uint32_t name) const noexcept { return pms_.net_qty(1, name); }

    int64_t open_orders() const noexcept { return oms_.active(); }

    void counters(FlowCounters& out) const noexcept {
        out = FlowCounters{};
        out.quotes = quotes_.load(std::memory_order_relaxed);
        out.submitted = submitted_;
        out.acked = acked_;
        out.fills = fills_;
        out.rejects = rejects_;
        out.cancels = cancels_;
        out.rms_reject = rms_reject_;
        out.daily_notional = daily_notional_;
        out.last_px = last_px_.load(std::memory_order_relaxed);
        out.last_tick_us = last_us_.load(std::memory_order_relaxed);
        out.parents_closed = parents_closed_;
        out.live_orders = oms_.active();
        out.rate_1s = rate_count_;
        const int64_t mark = out.last_px;
        int64_t unreal = 0;
        for (uint32_t nm = 1; nm <= kMaxNames; ++nm) {
            const PmsPos* p = pms_.find(1, nm);
            if (!p) continue;
            out.realized += p->realized;
            out.fees += p->fees;
            int64_t u = 0;
            if (mark > 0 && pms_.unrealized(1, nm, mark, u)) unreal += u;
        }
        out.unrealized = unreal;
    }

    int positions(FlowPos* out, int cap) const noexcept {
        if (!out || cap <= 0) return 0;
        const int64_t mark = last_px_.load(std::memory_order_relaxed);
        int n = 0;
        for (uint32_t nm = 1; nm <= kMaxNames && n < cap; ++nm) {
            const PmsPos* p = pms_.find(1, nm);
            if (!p) continue;
            FlowPos& f = out[n++];
            f.name = p->name;
            f.long_qty = p->long_qty;
            f.short_qty = p->short_qty;
            f.long_yday = p->long_yday;
            f.short_yday = p->short_yday;
            f.avg_long_px = p->avg_long_px;
            f.avg_short_px = p->avg_short_px;
            f.realized = p->realized;
            f.unrealized = 0;
            if (mark > 0) (void)pms_.unrealized(1, nm, mark, f.unrealized);
        }
        return n;
    }

    int recent_orders(FlowOrderRow* out, int cap) const noexcept {
        if (!out || cap <= 0) return 0;
        const int n = recent_n_ < cap ? recent_n_ : cap;
        for (int i = 0; i < n; ++i) {
            out[i] = recent_[(recent_head_ - n + i + kRecentOrders) % kRecentOrders];
        }
        return n;
    }

    // Drops the working parent and stops new slices. IOC children expire on their own.
    void cancel_all() noexcept {
        if (active_parent_ != 0) (void)ems_.close(active_parent_);
        active_parent_ = 0;
        parent_lost_ = 0;
        has_parent_.store(0, std::memory_order_release);
        want_side_.store(0, std::memory_order_release);
    }

    // Manual intents share the OMS, PMS, RMS, and id mint with the strategy.
    // Returns 0 accepted, 1 risk rejected, 2 capacity or transport failure.
    int submit_manual(const Intent& in, int64_t now_us) noexcept {
        if (!open_ || in.tag == 0 || !known_side(in.side) || in.qty <= 0 || in.px <= 0) return 2;
        if (child_index(in.tag) >= 0) return 2;
        if (in.px > INT64_MAX / in.qty) return 2;
        RmsReq req{};
        req.acct = in.acct;
        req.name = in.name;
        req.side = in.side;
        req.px = in.px;
        req.qty = in.qty;
        req.last_px = last_px_.load(std::memory_order_relaxed);
        req.daily_notional = daily_notional_;
        req.orders_in_window = rate_count_;
        req.live_orders = oms_.active();
        req.free_cash = 100000000;
        req.pos = pms_.find(in.acct, in.name);
        const RmsCode code = rms_.check(req);
        if (code != RmsCode::Ok) {
            ++rms_reject_;
            if (log_) log_->log(1, lit("[flow] manual rms reject "), static_cast<int>(code));
            return 1;
        }
        const int ci = alloc_child();
        if (ci < 0) {
            ++send_fail_;
            return 2;
        }
        OmsOrder draft{};
        draft.local = in.tag;
        draft.client = in.tag;
        draft.parent = 0;
        draft.name = in.name;
        draft.acct = in.acct;
        draft.venue = 1;
        draft.px = in.px;
        draft.qty = in.qty;
        draft.side = in.side;
        draft.type = 1;
        draft.tif = 3;
        draft.created_us = now_us;
        draft.updated_us = now_us;
        if (!oms_.submit(draft)) {
            ++send_fail_;
            return 2;
        }
        ++submitted_;
        const int rc = send_ ? send_(send_user_, in) : -1;
        if (rc != 0) {
            ++send_fail_;
            (void)oms_.reject(in.tag, now_us);
            return 2;
        }
        child_[ci] = Child{};
        child_[ci].tag = in.tag;
        child_[ci].parent = 0;
        child_[ci].local = in.tag;
        child_[ci].qty = in.qty;
        child_[ci].name = in.name;
        child_[ci].side = in.side;
        child_[ci].venue = 1;
        child_[ci].used = 1;
        uint64_t c = 0;
        if (cycles_ordered(c)) child_[ci].send_cycles = c;
        daily_notional_ += in.px * in.qty;
        ++rate_count_;
        push_order(in.tag, in.name, in.side, in.qty, 0, in.px, now_us);
        FlowNote note{};
        note.tag = in.tag;
        note.px = in.px;
        note.qty = in.qty;
        note.name = in.name;
        note.kind = static_cast<uint8_t>(WalKind::Intent);
        wal_note(note, now_us);
        return 0;
    }

    // MD callback thread.
    void on_tick(uint32_t name, const Quote& q, uint64_t t0_cycles, uint64_t t1_cycles) noexcept {
        if (!open_ || name != 1) return;
        int64_t px = q.last_px > 0 ? q.last_px : (q.bid_px > 0 && q.ask_px > 0 ? (q.bid_px + q.ask_px) / 2 : 0);
        if (px <= 0) return;
        const int64_t now = q.local_us > 0 ? q.local_us : last_local_us();
        int64_t fwd = 0;
        if (prev_px_ > 0) fwd = (px - prev_px_) * 1000000 / prev_px_;
        if (!pipe_.on_sample(px, now, fwd)) return;
        prev_px_ = px;
        (void)pipe_.on_quote(q.bid_px, q.ask_px, q.bid_qty, q.ask_qty, now);
        samples_.fetch_add(1, std::memory_order_relaxed);
        quotes_.fetch_add(1, std::memory_order_relaxed);

        bid_px_.store(q.bid_px, std::memory_order_relaxed);
        ask_px_.store(q.ask_px, std::memory_order_relaxed);
        bid_qty_.store(q.bid_qty, std::memory_order_relaxed);
        ask_qty_.store(q.ask_qty, std::memory_order_relaxed);
        last_px_.store(px, std::memory_order_relaxed);
        last_us_.store(now, std::memory_order_relaxed);
        if (t0_cycles != 0 && t1_cycles > t0_cycles) {
            tail_note(tail_, TailLane::Quote, t1_cycles - t0_cycles);
        }

        SigNote sig{};
        if (!pipe_.signal(0, signal_ppm_, sig, name, cfg_kind_ == static_cast<uint8_t>(FactorKind::Ofi) ? "ofi" : "sig",
                          now))
            return;
        mom_ppm_.store(sig.value, std::memory_order_relaxed);
        int64_t ofi_v = 0;
        if (confirm_slot_ >= 0) {
            SigNote ofi{};
            if (pipe_.signal(confirm_slot_, 0, ofi, name, "ofi", now)) ofi_v = ofi.value;
        }
        ofi_ppm_.store(confirm_slot_ >= 0 ? ofi_v : 0, std::memory_order_relaxed);
        if (has_parent_.load(std::memory_order_acquire) != 0) return;
        if (now - last_parent_us_.load(std::memory_order_relaxed) < parent_gap_us_) return;
        if (confirm_slot_ >= 0 && ofi_v != 0 && ((ofi_v > 0) != (sig.value > 0))) return;
        want_name_.store(name, std::memory_order_relaxed);
        want_stamp_.store(now, std::memory_order_relaxed);
        const int64_t dir = sign_ > 0 ? sig.value : -sig.value;
        want_side_.store(dir > 0 ? static_cast<uint8_t>(Side::Buy) : static_cast<uint8_t>(Side::Sell),
                         std::memory_order_release);
    }

    // Main thread. Opens parents, slices, routes, checks risk, sends, and drains CTP events.
    int on_beat(int64_t now_us) noexcept {
        if (!open_) return 0;
        drain_ctp(now_us);
        rotate_rate_window(now_us);
        if (ops_) (void)ops_(ops_user_, now_us, *this);

        if (has_parent_.load(std::memory_order_acquire) == 0 && (!control_ || control_->allow_new_orders())) {
            const uint8_t side = want_side_.load(std::memory_order_acquire);
            if (side != 0 && now_us - want_stamp_.load(std::memory_order_relaxed) <= 5000000 &&
                top_ready(side)) {
                const int64_t day_sec = now_us / 1000000;
                const uint64_t parent_id = mint_.next(day_index(now_us), static_cast<int>(day_sec % 86400));
                if (parent_id != 0 &&
                    ems_.open(parent_id, kParentQty, horizon_us_, interval_us_, 1, now_us, side)) {
                    active_parent_ = parent_id;
                    parent_side_ = side;
                    parent_name_ = want_name_.load(std::memory_order_relaxed);
                    parent_lost_ = 0;
                    has_parent_.store(1, std::memory_order_release);
                    last_parent_us_.store(now_us, std::memory_order_release);
                    want_side_.store(0, std::memory_order_release);
                    FlowNote n{};
                    n.tag = parent_id;
                    n.name = parent_name_;
                    n.kind = static_cast<uint8_t>(WalKind::Mark);
                    wal_note(n, now_us);
                    if (log_) log_->log(2, lit("[flow] parent "), parent_id, lit(" side="), static_cast<int>(side),
                                        lit(" name="), parent_name_);
                } else {
                    fail("parent open");
                    want_side_.store(0, std::memory_order_release);
                }
            }
        }

        int sent = 0;
        if (has_parent_.load(std::memory_order_acquire) != 0) {
            VenueTop v[2]{};
            int nv = 0;
            const int64_t bid = bid_px_.load(std::memory_order_relaxed);
            const int64_t ask = ask_px_.load(std::memory_order_relaxed);
            const int64_t bq = bid_qty_.load(std::memory_order_relaxed);
            const int64_t aq = ask_qty_.load(std::memory_order_relaxed);
            v[0].venue = 1;
            v[0].bid_px = bid;
            v[0].ask_px = ask;
            v[0].bid_qty = bq;
            v[0].ask_qty = aq;
            v[0].taker_fee_bps = 2;
            v[0].venue_limit = 0;
            v[0].lot = 1;
            nv = 1;
            if (mode_ == 2 || mode_ == 3) {
                v[1] = v[0];
                v[1].venue = 2;
                v[1].taker_fee_bps = 0;
                v[1].ask_qty = aq > 2 ? 2 : aq;
                v[1].bid_qty = bq > 2 ? 2 : bq;
                nv = 2;
            }
            const int got = ems_.poll(active_parent_, now_us, v, nv, slices_, 8);
            for (int i = 0; i < got; ++i) sent += send_child(slices_[i], now_us);
        }

        if (has_parent_.load(std::memory_order_acquire) != 0) {
            int64_t done = 0;
            int64_t working = 0;
            if (ems_.progress(active_parent_, done, working) && working == 0 &&
                done + parent_lost_ >= kParentQty) {
                if (!ems_.close(active_parent_) && log_) log_->log(1, lit("[flow] ems close refused "), active_parent_);
                FlowNote n{};
                n.tag = active_parent_;
                n.name = parent_name_;
                n.kind = static_cast<uint8_t>(WalKind::Mark);
                wal_note(n, now_us);
                ++parents_closed_;
                has_parent_.store(0, std::memory_order_release);
                active_parent_ = 0;
                parent_lost_ = 0;
                publish_snap(now_us);
                if (log_) log_->log(2, lit("[flow] parent closed total="), parents_closed_);
            }
        }

        if (now_us - last_snap_us_ >= 1000000) publish_snap(now_us);
        return sent;
    }

    int drain_ctp(int64_t now_us) noexcept {
        int n = 0;
        CtpEvent e{};
        while (queue_.pop(e)) {
            ++n;
            if (e.type == 1) {
                const int ci = child_index(e.tag);
                if (ci < 0) continue;
                if (oms_.ack(child_[ci].local, e.tag, child_[ci].venue, now_us)) {
                    ++acked_;
                    if (e.t1_cycles > child_[ci].send_cycles) {
                        tail_note(tail_, TailLane::Report, e.t1_cycles - child_[ci].send_cycles);
                    }
                    FlowNote note{};
                    note.tag = e.tag;
                    note.name = child_[ci].name;
                    note.kind = static_cast<uint8_t>(WalKind::Report);
                    wal_note(note, now_us);
                }
            } else if (e.type == 2) {
                const int ci = child_index(e.tag);
                if (ci < 0) continue;
                apply_fill(ci, e.px, e.qty, now_us);
            } else if (e.type == 3) {
                done_child(e.tag, e.filled, now_us);
            } else if (e.type == 4) {
                reject_child(e.tag, now_us, e.code);
            }
        }
        return n;
    }

    // Paper path. Called from the strategy engine on the main thread.
    void on_report(const Report& r, int64_t now_us) noexcept {
        const int ci = child_index(r.tag);
        if (ci < 0) return;
        const uint8_t phase = r.phase;
        if (phase == static_cast<uint8_t>(Phase::Working)) {
            if (oms_.ack(child_[ci].local, r.tag, child_[ci].venue, now_us)) {
                ++acked_;
                FlowNote note{};
                note.tag = r.tag;
                note.name = child_[ci].name;
                note.kind = static_cast<uint8_t>(WalKind::Report);
                wal_note(note, now_us);
            }
            return;
        }
        if (phase == static_cast<uint8_t>(Phase::Partial) || phase == static_cast<uint8_t>(Phase::Filled)) {
            if (r.qty > 0) apply_fill(ci, r.px, r.qty, now_us);
            if (phase == static_cast<uint8_t>(Phase::Filled)) finish_child(r.tag, now_us);
            return;
        }
        if (phase == static_cast<uint8_t>(Phase::Denied)) {
            reject_child(r.tag, now_us, static_cast<int>(r.why));
            return;
        }
        if (phase == static_cast<uint8_t>(Phase::Cancelled) || phase == static_cast<uint8_t>(Phase::Expired)) {
            finish_child(r.tag, now_us);
        }
    }

    // CTP callback thread. Only pushes fixed events; all state stays on the main thread.
    void push_ack(uint64_t tag, uint64_t t1_cycles, uint64_t gate_ns, int64_t now_us) noexcept {
        CtpEvent e{};
        e.tag = tag;
        e.type = 1;
        e.t1_cycles = t1_cycles;
        e.gate_ns = gate_ns;
        e.now_us = now_us;
        if (!queue_.push(e)) ++event_drops_;
    }

    void push_trade(uint64_t tag, int64_t px, int64_t qty, int64_t now_us) noexcept {
        CtpEvent e{};
        e.tag = tag;
        e.px = px;
        e.qty = qty;
        e.type = 2;
        e.now_us = now_us;
        if (!queue_.push(e)) ++event_drops_;
    }

    void push_done(uint64_t tag, int64_t filled, int status, int64_t now_us) noexcept {
        CtpEvent e{};
        e.tag = tag;
        e.filled = filled;
        e.code = status;
        e.type = 3;
        e.now_us = now_us;
        if (!queue_.push(e)) ++event_drops_;
    }

    void push_reject(uint64_t tag, int err_id, int64_t now_us) noexcept {
        CtpEvent e{};
        e.tag = tag;
        e.code = err_id;
        e.type = 4;
        e.now_us = now_us;
        if (!queue_.push(e)) ++event_drops_;
    }

    bool complete() const noexcept { return parents_closed_ >= 1 && fills_ > 0; }
    bool failed() const noexcept { return failed_ != 0; }
    bool pass() const noexcept { return result_pass_; }

    void fail_timeout() noexcept {
        if (!failed_) fail("timeout");
    }

    void summarize(WireLog* log) {
        if (!open_) {
            std::printf("full flow not open\n");
            return;
        }
        const uint64_t appended = wal_tail(wal_);
        int recovered = 0;
        if (wal_open_) recovered = wal_recover(wal_, [](const WalRec&) {});
        wal_appended_ = appended;
        wal_recovered_ = recovered < 0 ? 0 : static_cast<uint64_t>(recovered);
        wal_match_ = wal_appended_ > 0 && wal_appended_ == wal_recovered_;
        FlowSnap latest{};
        snap_match_ = snap_ && snap_->latest(latest) && latest.seq == snap_published_;
        snap_latest_ = latest.seq;

        int64_t realized = 0;
        int64_t fees = 0;
        int64_t unreal = 0;
        int64_t mark = last_px_.load(std::memory_order_relaxed);
        for (uint32_t nm = 1; nm <= 2; ++nm) {
            const PmsPos* p = pms_.find(1, nm);
            if (!p) continue;
            realized += p->realized;
            fees += p->fees;
            int64_t u = 0;
            if (mark > 0 && pms_.unrealized(1, nm, mark, u)) unreal += u;
        }

        FactorEval<1024>::Eval ev{};
        const bool ev_ok = pipe_.eval(ev);
        LatReport qr{};
        LatReport rr{};
        const bool q_ok = tail_report(tail_, TailLane::Quote, qr);
        const bool r_ok = tail_report(tail_, TailLane::Report, rr);

        const char* mode_label = mode_ == 3 ? "backtest" : (mode_ == 2 ? "paper" : "live");
        std::printf("\n=== pitlane %s simnow full flow (%s) ===\n", kVersion, mode_label);
        std::printf("oms     submitted=%lld acked=%lld filled_orders=%lld rejects=%lld cancels=%lld active=%d size=%d\n",
                    static_cast<long long>(submitted_), static_cast<long long>(acked_),
                    static_cast<long long>(fills_), static_cast<long long>(rejects_),
                    static_cast<long long>(cancels_), oms_.active(), oms_.size());
        std::printf("pms     net=%lld realized=%lld fees=%lld unrealized=%lld rows=%d\n",
                    static_cast<long long>(pms_.net_qty(1, 1)), static_cast<long long>(realized),
                    static_cast<long long>(fees), static_cast<long long>(unreal), pms_.size());
        std::printf("ems     parents_closed=%lld lost=%lld working_parent=%d\n",
                    static_cast<long long>(parents_closed_), static_cast<long long>(parent_lost_total_),
                    has_parent_.load(std::memory_order_relaxed));
        std::printf("study   kind=%u window=%u sign=%d signal_ppm=%lld horizon_us=%lld\n",
                    static_cast<unsigned>(cfg_kind_), static_cast<unsigned>(cfg_window_), static_cast<int>(sign_),
                    static_cast<long long>(signal_ppm_), static_cast<long long>(horizon_us_));
        PerfCurve<4096>::Stats ps{};
        if (perf_.result(ps))
            std::printf("perf    n=%lld start=%lld end=%lld peak=%lld min=%lld maxdd=%lld maxdd_ppm=%lld sharpe_ppm=%lld\n",
                        static_cast<long long>(ps.n), static_cast<long long>(ps.start), static_cast<long long>(ps.end),
                        static_cast<long long>(ps.peak), static_cast<long long>(ps.min),
                        static_cast<long long>(ps.max_drawdown), static_cast<long long>(ps.max_drawdown_ppm),
                        static_cast<long long>(ps.sharpe_ppm));
        else
            std::printf("perf    n=%d (insufficient samples)\n", perf_.size());
        std::printf("cost    turnover=%lld fees=%lld parents_lost=%lld zero_fill=%lld send_fail=%lld rms_reject=%lld\n",
                    static_cast<long long>(daily_notional_), static_cast<long long>(fees),
                    static_cast<long long>(parent_lost_total_), static_cast<long long>(zero_fill_),
                    static_cast<long long>(send_fail_), static_cast<long long>(rms_reject_));
        if (ev_ok)
            std::printf("factor  samples=%lld quotes=%lld eval_n=%lld ic_ppm=%lld rank_ic=%lld hit=%lld mom=%lld ofi=%lld\n",
                        static_cast<long long>(samples_), static_cast<long long>(quotes_),
                        static_cast<long long>(ev.n), static_cast<long long>(ev.ic_ppm),
                        static_cast<long long>(ev.rank_ic_ppm), static_cast<long long>(ev.hit_rate_ppm),
                        static_cast<long long>(mom_ppm_.load()), static_cast<long long>(ofi_ppm_.load()));
        else
            std::printf("factor  samples=%lld quotes=%lld eval_n=%d mom=%lld ofi=%lld\n",
                        static_cast<long long>(samples_), static_cast<long long>(quotes_), pipe_.eval_size(),
                        static_cast<long long>(mom_ppm_.load()), static_cast<long long>(ofi_ppm_.load()));
        std::printf("wal     appended=%llu recovered=%llu match=%s\n",
                    static_cast<unsigned long long>(wal_appended_), static_cast<unsigned long long>(wal_recovered_),
                    wal_match_ ? "yes" : "no");
        std::printf("snap    published=%llu latest_seq=%llu match=%s\n",
                    static_cast<unsigned long long>(snap_published_), static_cast<unsigned long long>(snap_latest_),
                    snap_match_ ? "yes" : "no");
        if (q_ok)
            std::printf("tail    quote n=%llu p50=%llu p99=%llu p999=%llu max=%llu\n",
                        static_cast<unsigned long long>(qr.n), static_cast<unsigned long long>(qr.p50),
                        static_cast<unsigned long long>(qr.p99), static_cast<unsigned long long>(qr.p999),
                        static_cast<unsigned long long>(qr.max));
        else
            std::printf("tail    quote n=0 (paper path has no user-space cycles)\n");
        if (r_ok)
            std::printf("tail    report n=%llu p50=%llu p99=%llu p999=%llu max=%llu\n",
                        static_cast<unsigned long long>(rr.n), static_cast<unsigned long long>(rr.p50),
                        static_cast<unsigned long long>(rr.p99), static_cast<unsigned long long>(rr.p999),
                        static_cast<unsigned long long>(rr.max));
        else
            std::printf("tail    report n=0\n");
        std::printf("events  drops=%lld zero_fill=%lld send_fail=%lld rms_reject=%lld\n",
                    static_cast<long long>(event_drops_), static_cast<long long>(zero_fill_),
                    static_cast<long long>(send_fail_), static_cast<long long>(rms_reject_));
        if (failed_) {
            std::printf("result  FAIL %s\n", fail_reason_ ? fail_reason_ : "error");
        } else if (fills_ > 0 && parents_closed_ >= 1 && wal_match_ && snap_match_ && oms_.size() > 0 &&
                   pms_.size() > 0) {
            result_pass_ = true;
            std::printf("result  PASS\n");
        } else {
            std::printf("result  FAIL invariants (fills=%lld parents=%lld wal=%d snap=%d oms=%d pms=%d)\n",
                        static_cast<long long>(fills_), static_cast<long long>(parents_closed_), wal_match_ ? 1 : 0,
                        snap_match_ ? 1 : 0, oms_.size(), pms_.size());
        }
        std::printf("==============================================\n");
        if (log) {
            log->log(result_pass_ ? 2 : 0, lit("[flow] result "), result_pass_ ? lit("PASS") : lit("FAIL"),
                     lit(" fills="), static_cast<int>(fills_), lit(" parents="), static_cast<int>(parents_closed_));
        }
    }

private:
    struct Child {
        uint64_t tag{0};
        uint64_t parent{0};
        uint64_t local{0};
        uint64_t send_cycles{0};
        int64_t qty{0};
        int64_t filled{0};
        uint32_t name{0};
        uint8_t side{0};
        uint8_t venue{0};
        uint8_t used{0};
        uint8_t done_seen{0};
        int64_t done_expect{0};
    };

    struct CtpEvent {
        uint64_t tag{0};
        int64_t px{0};
        int64_t qty{0};
        int64_t filled{0};
        int64_t now_us{0};
        uint64_t t1_cycles{0};
        uint64_t gate_ns{0};
        int code{0};
        uint8_t type{0};
        uint8_t status{0};
    };

    struct EventQueue {
        CtpEvent slot[kEventCap];
        std::atomic<uint32_t> head{0};
        std::atomic<uint32_t> tail{0};
        std::atomic_flag lock = ATOMIC_FLAG_INIT;

        bool push(const CtpEvent& e) noexcept {
            while (lock.test_and_set(std::memory_order_acquire)) cpu_relax();
            const uint32_t h = head.load(std::memory_order_relaxed);
            const uint32_t t = tail.load(std::memory_order_relaxed);
            if (h - t >= static_cast<uint32_t>(kEventCap)) {
                lock.clear(std::memory_order_release);
                return false;
            }
            slot[h % kEventCap] = e;
            head.store(h + 1, std::memory_order_release);
            lock.clear(std::memory_order_release);
            return true;
        }

        bool pop(CtpEvent& e) noexcept {
            const uint32_t t = tail.load(std::memory_order_relaxed);
            const uint32_t h = head.load(std::memory_order_acquire);
            if (t == h) return false;
            e = slot[t % kEventCap];
            tail.store(t + 1, std::memory_order_release);
            return true;
        }
    };

    static int day_index(int64_t now_us) noexcept {
        const time_t sec = static_cast<time_t>(now_us / 1000000);
        std::tm tm{};
        localtime_r(&sec, &tm);
        return tm.tm_yday;
    }

    int child_index(uint64_t tag) const noexcept {
        for (int i = 0; i < kChildCap; ++i) {
            if (child_[i].used && child_[i].tag == tag) return i;
        }
        return -1;
    }

    bool top_ready(uint8_t side) const noexcept {
        if (side == static_cast<uint8_t>(Side::Buy))
            return ask_px_.load(std::memory_order_relaxed) > 0 && ask_qty_.load(std::memory_order_relaxed) > 0;
        return bid_px_.load(std::memory_order_relaxed) > 0 && bid_qty_.load(std::memory_order_relaxed) > 0;
    }

    int alloc_child() noexcept {
        for (int i = 0; i < kChildCap; ++i) {
            if (!child_[i].used) return i;
            const OmsOrder* o = oms_.find_local(child_[i].local);
            if (o && (o->phase == static_cast<uint8_t>(Phase::Filled) ||
                      o->phase == static_cast<uint8_t>(Phase::Cancelled) ||
                      o->phase == static_cast<uint8_t>(Phase::Denied) ||
                      o->phase == static_cast<uint8_t>(Phase::Expired))) {
                child_[i] = Child{};
                return i;
            }
        }
        return -1;
    }

    int send_child(const SorSlice& s, int64_t now_us) noexcept {
        const int ci = alloc_child();
        if (ci < 0) {
            ++send_fail_;
            return 0;
        }
        const int64_t sec = now_us / 1000000;
        const uint64_t tag = mint_.next(day_index(now_us), static_cast<int>(sec % 86400));
        if (tag == 0) return 0;

        Intent in{};
        in.tag = tag;
        in.name = parent_name_;
        in.acct = 1;
        in.px = s.px;
        in.qty = s.qty;
        in.stamp_us = now_us;
        in.side = parent_side_;
        in.act = static_cast<uint8_t>(Act::Enter);
        in.style = static_cast<uint8_t>(Style::Immediate);

        RmsReq req{};
        req.acct = 1;
        req.name = parent_name_;
        req.side = parent_side_;
        req.px = s.px;
        req.qty = s.qty;
        req.last_px = last_px_.load(std::memory_order_relaxed);
        req.daily_notional = daily_notional_;
        req.orders_in_window = rate_count_;
        req.live_orders = oms_.active();
        req.free_cash = 100000000;
        req.pos = pms_.find(1, parent_name_);
        const RmsCode code = rms_.check(req);
        if (code != RmsCode::Ok) {
            ++rms_reject_;
            (void)ems_.on_reject(active_parent_, s.qty);
            parent_lost_ += s.qty;
            parent_lost_total_ += s.qty;
            if (log_) log_->log(1, lit("[flow] rms reject "), static_cast<int>(code), lit(" qty="), s.qty);
            return 0;
        }

        OmsOrder draft{};
        draft.local = tag;
        draft.client = tag;
        draft.parent = active_parent_;
        draft.name = parent_name_;
        draft.acct = 1;
        draft.venue = s.venue;
        draft.px = s.px;
        draft.qty = s.qty;
        draft.side = parent_side_;
        draft.type = 1;
        draft.tif = 3;
        draft.created_us = now_us;
        draft.updated_us = now_us;
        if (!oms_.submit(draft)) {
            ++send_fail_;
            (void)ems_.on_reject(active_parent_, s.qty);
            parent_lost_ += s.qty;
            parent_lost_total_ += s.qty;
            return 0;
        }
        ++submitted_;

        int rc = -1;
        if (send_) rc = send_(send_user_, in);
        if (rc != 0) {
            ++send_fail_;
            (void)oms_.reject(tag, now_us);
            (void)ems_.on_reject(active_parent_, s.qty);
            parent_lost_ += s.qty;
            parent_lost_total_ += s.qty;
            return 0;
        }

        child_[ci] = Child{};
        child_[ci].tag = tag;
        child_[ci].parent = active_parent_;
        child_[ci].local = tag;
        child_[ci].qty = s.qty;
        child_[ci].name = parent_name_;
        child_[ci].side = parent_side_;
        child_[ci].venue = static_cast<uint8_t>(s.venue);
        child_[ci].used = 1;
        uint64_t c = 0;
        if (cycles_ordered(c)) child_[ci].send_cycles = c;
        daily_notional_ += s.px * s.qty;
        ++rate_count_;
        push_order(tag, parent_name_, parent_side_, s.qty, 0, s.px, now_us);
        FlowNote note{};
        note.tag = tag;
        note.px = s.px;
        note.qty = s.qty;
        note.name = parent_name_;
        note.kind = static_cast<uint8_t>(WalKind::Intent);
        wal_note(note, now_us);
        return 1;
    }

    void apply_fill(int ci, int64_t px, int64_t qty, int64_t now_us) noexcept {
        if (!child_[ci].used || qty <= 0 || px <= 0) return;
        const OmsOrder* live_order = oms_.find_local(child_[ci].local);
        if (live_order && live_order->phase == static_cast<uint8_t>(Phase::Staged)) {
            // Some venues report the first fill without a separate accepted event.
            if (oms_.ack(child_[ci].local, child_[ci].tag, child_[ci].venue, now_us)) ++acked_;
        }
        OmsFill f{};
        if (!oms_.fill(child_[ci].local, static_cast<uint64_t>(++fill_seq_), qty, px, now_us, f)) {
            if (log_) log_->log(0, lit("[flow] oms fill refused tag="), child_[ci].tag, lit(" qty="), qty);
            return;
        }
        PmsFill pf{};
        pf.acct = 1;
        pf.name = child_[ci].name;
        pf.side = child_[ci].side;
        pf.qty = f.qty;
        pf.px = f.px;
        pf.fee = f.px * f.qty * 2 / 10000;
        pf.close_qty = 0;
        if (!pms_.apply(pf) && log_) log_->log(0, lit("[flow] pms apply refused name="), pf.name);
        child_[ci].filled += f.qty;
        (void)ems_.on_child(child_[ci].parent, child_[ci].tag, f.qty, f.leaves == 0);
        ++fills_;
        push_order(child_[ci].tag, child_[ci].name, child_[ci].side, child_[ci].qty, child_[ci].filled, f.px, now_us);
        publish_snap(now_us);
        FlowNote note{};
        note.tag = child_[ci].tag;
        note.px = f.px;
        note.qty = f.qty;
        note.name = child_[ci].name;
        note.kind = static_cast<uint8_t>(WalKind::Report);
        wal_note(note, now_us);
        // A terminal order report may arrive before its trade reports.
        const uint64_t tag = child_[ci].tag;
        if (child_[ci].done_seen && child_[ci].filled >= child_[ci].done_expect) finish_child(tag, now_us);
    }

    void done_child(uint64_t tag, int64_t filled, int64_t now_us) noexcept {
        const int ci = child_index(tag);
        if (ci < 0) return;
        if (filled <= 0 || child_[ci].filled >= filled) {
            finish_child(tag, now_us);
            return;
        }
        child_[ci].done_seen = 1;
        child_[ci].done_expect = filled;
    }

    void finish_child(uint64_t tag, int64_t now_us) noexcept {
        const int ci = child_index(tag);
        if (ci < 0) return;
        OmsOrder* o = oms_.find_local(child_[ci].local);
        if (o) {
            if (o->phase == static_cast<uint8_t>(Phase::Working) || o->phase == static_cast<uint8_t>(Phase::Partial)) {
                if (oms_.cancel(o->local, now_us) && oms_.confirm_cancel(o->local, now_us)) ++cancels_;
            } else if (o->phase == static_cast<uint8_t>(Phase::Staged)) {
                if (oms_.reject(o->local, now_us)) ++rejects_;
            }
            if (child_[ci].filled == 0) ++zero_fill_;
        }
        const int64_t remaining = child_[ci].qty - child_[ci].filled;
        if (remaining > 0 && child_[ci].parent != 0) {
            (void)ems_.on_reject(child_[ci].parent, remaining);
            if (child_[ci].parent == active_parent_) {
                parent_lost_ += remaining;
                parent_lost_total_ += remaining;
            }
        }
        push_order(child_[ci].tag, child_[ci].name, child_[ci].side, child_[ci].qty, child_[ci].filled, 0, now_us);
        child_[ci] = Child{};
    }

    void reject_child(uint64_t tag, int64_t now_us, int code) noexcept {
        const int ci = child_index(tag);
        if (ci < 0) return;
        if (oms_.reject(child_[ci].local, now_us)) ++rejects_;
        if (child_[ci].parent != 0) {
            (void)ems_.on_reject(child_[ci].parent, child_[ci].qty);
            if (child_[ci].parent == active_parent_) {
                parent_lost_ += child_[ci].qty;
                parent_lost_total_ += child_[ci].qty;
            }
        }
        push_order(child_[ci].tag, child_[ci].name, child_[ci].side, child_[ci].qty, 0, 0, now_us);
        if (log_) log_->log(1, lit("[flow] reject tag="), tag, lit(" code="), code);
        child_[ci] = Child{};
    }

    void push_order(uint64_t tag, uint32_t name, uint8_t side, int64_t qty, int64_t filled, int64_t px,
                    int64_t now_us) noexcept {
        FlowOrderRow r{};
        r.tag = tag;
        r.name = name;
        r.side = side;
        if (filled <= 0) r.phase = static_cast<uint8_t>(Phase::Working);
        else if (filled >= qty) r.phase = static_cast<uint8_t>(Phase::Filled);
        else r.phase = static_cast<uint8_t>(Phase::Partial);
        r.qty = qty;
        r.filled = filled;
        r.px = px;
        r.updated_us = now_us;
        // One row per order: refresh in place, then move it to the newest slot.
        for (int i = 0; i < recent_n_; ++i) {
            const int idx = (recent_head_ - recent_n_ + i + kRecentOrders) % kRecentOrders;
            if (recent_[idx].tag != tag) continue;
            if (r.px == 0) r.px = recent_[idx].px;
            for (int j = i; j + 1 < recent_n_; ++j) {
                const int dst = (recent_head_ - recent_n_ + j + kRecentOrders) % kRecentOrders;
                const int src = (recent_head_ - recent_n_ + j + 1 + kRecentOrders) % kRecentOrders;
                recent_[dst] = recent_[src];
            }
            recent_[(recent_head_ - 1 + kRecentOrders) % kRecentOrders] = r;
            return;
        }
        recent_[recent_head_] = r;
        recent_head_ = (recent_head_ + 1) % kRecentOrders;
        if (recent_n_ < kRecentOrders) ++recent_n_;
    }

    void rotate_rate_window(int64_t now_us) noexcept {
        const int64_t sec = now_us / 1000000;
        if (sec != rate_sec_) {
            rate_sec_ = sec;
            rate_count_ = 0;
        }
    }

    void publish_snap(int64_t now_us) noexcept {
        if (!snap_) return;
        FlowSnap s{};
        s.seq = snap_->published() + 1;
        int64_t equity = 0;
        int64_t mark = last_px_.load(std::memory_order_relaxed);
        for (uint32_t nm = 1; nm <= 2; ++nm) {
            const PmsPos* p = pms_.find(1, nm);
            if (!p) continue;
            equity += p->realized;
            int64_t u = 0;
            if (mark > 0 && pms_.unrealized(1, nm, mark, u)) equity += u;
        }
        if (perf_.size() < 4096) (void)perf_.mark(equity);
        if (const PmsPos* p = pms_.find(1, 1)) {
            s.net_qty = p->long_qty - p->short_qty;
            s.realized = p->realized;
        }
        s.last_px = last_px_.load(std::memory_order_relaxed);
        s.momentum_ppm = mom_ppm_.load(std::memory_order_relaxed);
        s.ofi_ppm = ofi_ppm_.load(std::memory_order_relaxed);
        s.orders = static_cast<int32_t>(submitted_);
        s.fills = static_cast<int32_t>(fills_);
        s.parents_closed = static_cast<int32_t>(parents_closed_);
        s.payload_orders = oms_.size();
        snap_->publish(s);
        snap_published_ = s.seq;
        last_snap_us_ = now_us;
    }

    void wal_note(const FlowNote& n, int64_t now_us) noexcept {
        if (!wal_open_) return;
        (void)wal_append_item(wal_, static_cast<WalKind>(n.kind), n, static_cast<uint64_t>(now_us));
    }

    int64_t last_local_us() const noexcept { return last_us_.load(std::memory_order_relaxed); }

    void fail(const char* why) noexcept {
        if (!failed_) {
            failed_ = 1;
            fail_reason_ = why;
            if (log_) log_->log(0, lit("[flow] fail "), why);
        }
    }

    Oms<1024> oms_{};
    Pms<64> pms_{};
    Rms rms_{};
    Ems<8, 4> ems_{};
    FactorPipeline<4, 128, 1024> pipe_{};
    TailBoard tail_{};
    WalView wal_{};
    MapInfo snap_map_{};
    SnapRing<FlowSnap, 64>* snap_{nullptr};
    EventQueue queue_{};
    Child child_[kChildCap]{};
    FlowOrderRow recent_[kRecentOrders]{};
    int recent_head_{0};
    int recent_n_{0};
    SorSlice slices_[8]{};
    IdMint mint_{1, 1};

    SendFn send_{nullptr};
    void* send_user_{nullptr};
    ControlPlane* control_{nullptr};
    OpsBeatFn ops_{nullptr};
    void* ops_user_{nullptr};
    WireLog* log_{nullptr};
    PipeStage stage_{PipeStage::Live};
    int mode_{0};
    bool open_{false};
    bool wal_open_{false};

    char wal_path_[512]{};
    char snap_path_[512]{};

    std::atomic<int64_t> bid_px_{0};
    std::atomic<int64_t> ask_px_{0};
    std::atomic<int64_t> bid_qty_{0};
    std::atomic<int64_t> ask_qty_{0};
    std::atomic<int64_t> last_px_{0};
    std::atomic<int64_t> last_us_{0};
    std::atomic<int64_t> mom_ppm_{0};
    std::atomic<int64_t> ofi_ppm_{0};
    std::atomic<uint8_t> want_side_{0};
    std::atomic<uint32_t> want_name_{0};
    std::atomic<int64_t> want_stamp_{0};
    std::atomic<uint8_t> has_parent_{0};
    std::atomic<int64_t> last_parent_us_{0};

    int64_t prev_px_{0};
    int64_t signal_ppm_{10};
    int8_t sign_{1};
    uint8_t cfg_kind_{2};
    uint16_t cfg_window_{16};
    int32_t confirm_slot_{-1};
    PerfCurve<4096> perf_{};
    int64_t parent_gap_us_{5000000};
    int64_t horizon_us_{60000000};
    int64_t interval_us_{20000000};
    uint64_t active_parent_{0};
    uint32_t parent_name_{0};
    uint8_t parent_side_{0};
    int64_t parent_lost_{0};
    int64_t parent_lost_total_{0};
    int64_t daily_notional_{0};
    int32_t rate_count_{0};
    int64_t rate_sec_{0};
    int64_t last_snap_us_{0};
    uint64_t snap_published_{0};

    int64_t submitted_{0};
    int64_t acked_{0};
    int64_t fills_{0};
    int64_t rejects_{0};
    int64_t cancels_{0};
    int64_t zero_fill_{0};
    int64_t send_fail_{0};
    int64_t rms_reject_{0};
    int64_t parents_closed_{0};
    std::atomic<int64_t> samples_{0};
    std::atomic<int64_t> quotes_{0};
    int64_t event_drops_{0};
    uint64_t fill_seq_{0};

    uint64_t wal_appended_{0};
    uint64_t wal_recovered_{0};
    uint64_t snap_latest_{0};
    bool wal_match_{false};
    bool snap_match_{false};
    bool result_pass_{false};
    uint8_t failed_{0};
    const char* fail_reason_{nullptr};
};

} // namespace pitlane
