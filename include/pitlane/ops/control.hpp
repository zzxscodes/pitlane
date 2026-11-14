#pragma once

// Control plane. The trading thread drains the command channel once per beat,
// applies the state gate, and turns accepted requests into intents through the
// same manual book and send callback the strategy uses.

#include "pitlane/desk/ids.hpp"
#include "pitlane/order/manual.hpp"
#include "pitlane/order/risk_guard.hpp"
#include "pitlane/order/rms.hpp"
#include "pitlane/ops/command.hpp"
#include "pitlane/ops/policy.hpp"
#include "pitlane/ops/status.hpp"

#include <cstdint>
#include <cstdio>

namespace pitlane {

class ControlPlane {
public:
    using SendFn = ManualBook::SendFn;
    using TopFn = ManualTopFn;
    using NetFn = ManualBook::NetFn;
    using ReloadFn = bool (*)(void* user, OpsPolicy& out);
    using CancelFn = void (*)(void* user);

    void open(OpsChannel<256>* ch, Rms* rms, const OpsPolicy& p) noexcept {
        ch_ = ch;
        rms_ = rms;
        pol_ = p;
        started_ = false;
        paused_ = false;
        halted_ = false;
        risk_block_ = false;
        stopping_ = false;
        strategy_on_ = true;
        link_ok_ = true;
        cfg_ver_ = 1;
        note_[0] = 0;
        seen_n_ = 0;
        seen_head_ = 0;
        for (int i = 0; i < 64; ++i) seen_[i] = 0;
        for (int i = 0; i < 60; ++i) manual_bucket_[i] = Bucket{};
    }

    void set_account(uint32_t acct) noexcept {
        acct_ = acct;
        manual_.set_account(acct);
    }

    void set_mint(IdMint* mint) noexcept {
        mint_ = mint;
        started_ = false;
    }

    void set_send(SendFn fn, void* user) noexcept {
        send_ = fn;
        send_user_ = user;
    }

    void set_top(TopFn fn, void* user) noexcept {
        top_ = fn;
        top_user_ = user;
    }

    void set_net(NetFn fn, void* user) noexcept {
        net_ = fn;
        net_user_ = user;
    }

    void set_reload(ReloadFn fn, void* user) noexcept {
        reload_ = fn;
        reload_user_ = user;
    }

    void set_cancel(CancelFn fn, void* user) noexcept {
        cancel_ = fn;
        cancel_user_ = user;
    }

    // Trading thread. Returns the number of commands handled.
    int on_beat(int64_t now_us, const RiskInputs& in, StatusSnapshot& snap) noexcept {
        ensure_manual();
        link_ok_ = in.md_up && in.td_up;
        int handled = 0;
        OpsCommand c{};
        while (ch_ && ch_->take(c)) {
            ++handled;
            handle(c, now_us);
        }
        if (allow_new_orders()) manual_.on_beat(now_us, pol_.max_manual_qty, pol_.max_manual_notional);

        snap.state = state();
        snap.md_up = in.md_up ? 1 : 0;
        snap.td_up = in.td_up ? 1 : 0;
        snap.strategy_on = strategy_on_ ? 1 : 0;
        snap.halted = (halted_ || risk_block_) ? 1 : 0;
        snap.config_version = cfg_ver_;
        std::snprintf(snap.note, sizeof snap.note, "%s", note_);
        return handled;
    }

    void apply_risk(RiskAction action, const RiskEvent& ev, int64_t now_us) noexcept {
        if (action == RiskAction::Ok) {
            if (risk_block_ && !halted_) risk_block_ = false;
            return;
        }
        if (action == RiskAction::Warn) {
            std::snprintf(note_, sizeof note_, "warn: %s", ev.text);
            return;
        }
        risk_block_ = true;
        std::snprintf(note_, sizeof note_, "risk: %s", ev.text);
        if (action == RiskAction::Halt && !halted_) halt(now_us, ev.text);
    }

    bool allow_new_orders() const noexcept {
        return !paused_ && !halted_ && !risk_block_ && !stopping_ && strategy_on_ && link_ok_;
    }

    bool halted() const noexcept { return halted_ || risk_block_; }

    bool paused() const noexcept { return paused_; }

    bool stopping() const noexcept { return stopping_; }

    uint32_t config_version() const noexcept { return cfg_ver_; }

    uint8_t state() const noexcept {
        if (stopping_) return 4;
        if (halted_ || risk_block_) return 3;
        if (paused_) return 2;
        return 1;
    }

    void set_strategy_on(bool on) noexcept { strategy_on_ = on; }

    void note_report(bool reject) noexcept {
        ++reports_;
        if (reject) ++reports_rej_;
    }

    const OpsPolicy& policy() const noexcept { return pol_; }

    ManualBook& manual() noexcept { return manual_; }

private:
    struct Bucket {
        int64_t sec{0};
        int32_t n{0};
    };

    void ensure_manual() noexcept {
        if (started_) return;
        manual_.set_account(acct_);
        manual_.open(mint_, send_, send_user_, top_, top_user_, net_, net_user_);
        started_ = true;
    }

    void halt(int64_t now_us, const char* why) noexcept {
        halted_ = true;
        if (rms_) rms_->halt(true);
        manual_.cancel_all();
        if (cancel_) cancel_(cancel_user_);
        std::snprintf(note_, sizeof note_, "halted: %s", why ? why : "operator");
        (void)now_us;
    }

    bool seen(uint64_t seq) const noexcept {
        for (int i = 0; i < seen_n_; ++i) {
            if (seen_[i] == seq) return true;
        }
        return false;
    }

    void remember(uint64_t seq) noexcept {
        seen_[seen_head_] = seq;
        seen_head_ = (seen_head_ + 1) % 64;
        if (seen_n_ < 64) ++seen_n_;
    }

    bool manual_rate_ok(int64_t now_us) noexcept {
        const int64_t sec = now_us / 1000000;
        int32_t count = 0;
        for (int i = 0; i < 60; ++i) {
            if (sec - manual_bucket_[i].sec < 60) count += manual_bucket_[i].n;
        }
        return pol_.max_manual_per_min <= 0 || count < pol_.max_manual_per_min;
    }

    void manual_rate_add(int64_t now_us) noexcept {
        const int64_t sec = now_us / 1000000;
        Bucket& b = manual_bucket_[sec % 60];
        if (b.sec != sec) {
            b.sec = sec;
            b.n = 0;
        }
        ++b.n;
    }

    void handle(const OpsCommand& c, int64_t now_us) noexcept {
        OpsResult r{};
        r.seq = c.seq;
        r.id = c.id;
        if (c.seq == 0) {
            r.code = kOpsBadReq;
            std::snprintf(r.msg, sizeof r.msg, "missing seq");
            if (ch_) ch_->ack(r, now_us, c.kind);
            return;
        }
        if (seen(c.seq)) return;
        remember(c.seq);

        switch (static_cast<OpsKind>(c.kind)) {
            case OpsKind::Direct: r.code = do_direct(c, now_us); break;
            case OpsKind::Basket: r.code = do_basket(c, now_us); break;
            case OpsKind::Algo: r.code = do_algo(c, now_us); break;
            case OpsKind::CancelAll:
                manual_.cancel_all();
                if (cancel_) cancel_(cancel_user_);
                std::snprintf(note_, sizeof note_, "cancel all");
                r.code = kOpsOk;
                break;
            case OpsKind::Pause:
                paused_ = true;
                std::snprintf(note_, sizeof note_, "paused");
                r.code = kOpsOk;
                break;
            case OpsKind::Resume:
                paused_ = false;
                std::snprintf(note_, sizeof note_, halted_ ? "resumed (halted)" : "running");
                r.code = kOpsOk;
                break;
            case OpsKind::Halt:
                halt(now_us, "operator");
                r.code = kOpsOk;
                break;
            case OpsKind::ResumeRisk:
                halted_ = false;
                risk_block_ = false;
                if (rms_) rms_->halt(false);
                std::snprintf(note_, sizeof note_, "risk resumed");
                r.code = kOpsOk;
                break;
            case OpsKind::Reload:
                r.code = do_reload();
                break;
            case OpsKind::Stop:
                stopping_ = true;
                manual_.cancel_all();
                if (cancel_) cancel_(cancel_user_);
                std::snprintf(note_, sizeof note_, "stopping");
                r.code = kOpsOk;
                break;
            default:
                r.code = kOpsUnknown;
                std::snprintf(r.msg, sizeof r.msg, "unknown kind");
                break;
        }
        if (r.code == kOpsOk && r.msg[0] == 0) std::snprintf(r.msg, sizeof r.msg, "ok");
        if (r.code != kOpsOk && r.msg[0] == 0) std::snprintf(r.msg, sizeof r.msg, "%s", ops_code_name(r.code));
        if (ch_) ch_->ack(r, now_us, c.kind);
    }

    int do_direct(const OpsCommand& c, int64_t now_us) noexcept {
        if (!allow_new_orders()) {
            std::snprintf(note_, sizeof note_, "manual rejected: %s", stopped_reason());
            return kOpsHalted;
        }
        if (!known_side(c.side) || c.name == 0 || c.qty <= 0) return kOpsBadReq;
        if (pol_.max_manual_qty > 0 && c.qty > pol_.max_manual_qty) return kOpsRisk;
        if (c.px > 0 && pol_.max_manual_notional > 0 && c.qty > pol_.max_manual_notional / c.px) return kOpsRisk;
        if (!manual_rate_ok(now_us)) return kOpsRisk;
        const int rc = manual_.direct(acct_, c.name, c.side, c.qty, c.px, now_us);
        if (rc == kOpsOk) manual_rate_add(now_us);
        return rc;
    }

    int do_basket(const OpsCommand& c, int64_t now_us) noexcept {
        if (!allow_new_orders()) return kOpsHalted;
        if (c.legs == 0 || c.legs > kOpsBasketLegs) return kOpsBadReq;
        for (int i = 0; i < c.legs; ++i) {
            if (c.basket[i].name == 0) return kOpsBadReq;
            if (c.basket[i].target > 1000000 || c.basket[i].target < -1000000) return kOpsRisk;
        }
        if (!manual_rate_ok(now_us)) return kOpsRisk;
        const int rc = manual_.basket_open(c.id, c.basket, c.legs, now_us);
        if (rc == kOpsOk) manual_rate_add(now_us);
        return rc;
    }

    int do_algo(const OpsCommand& c, int64_t now_us) noexcept {
        if (!allow_new_orders()) return kOpsHalted;
        if (!known_side(c.side) || c.name == 0 || c.qty <= 0) return kOpsBadReq;
        if (pol_.max_manual_qty > 0 && c.qty > pol_.max_manual_qty) return kOpsRisk;
        if (c.horizon_us <= 0 || c.slice_us <= 0 || c.slice_us > c.horizon_us) return kOpsBadReq;
        if (!manual_rate_ok(now_us)) return kOpsRisk;
        const int rc = manual_.algo_open(c.id, c.name, c.side, c.qty, c.horizon_us, c.slice_us, now_us);
        if (rc == kOpsOk) manual_rate_add(now_us);
        return rc;
    }

    int do_reload() noexcept {
        if (!reload_) return kOpsBadReq;
        OpsPolicy next{};
        if (!reload_(reload_user_, next)) return kOpsBadReq;
        pol_ = next;
        ++cfg_ver_;
        std::snprintf(note_, sizeof note_, "reloaded v%u", static_cast<unsigned>(cfg_ver_));
        return kOpsOk;
    }

    const char* stopped_reason() const noexcept {
        if (stopping_) return "stopping";
        if (halted_) return "halted";
        if (risk_block_) return "risk";
        if (!link_ok_) return "link down";
        if (!strategy_on_) return "strategy off";
        return "paused";
    }

    OpsChannel<256>* ch_{nullptr};
    Rms* rms_{nullptr};
    OpsPolicy pol_{};
    ManualBook manual_{};
    IdMint* mint_{nullptr};
    SendFn send_{nullptr};
    void* send_user_{nullptr};
    TopFn top_{nullptr};
    void* top_user_{nullptr};
    NetFn net_{nullptr};
    void* net_user_{nullptr};
    ReloadFn reload_{nullptr};
    void* reload_user_{nullptr};
    CancelFn cancel_{nullptr};
    void* cancel_user_{nullptr};

    uint32_t acct_{1};
    bool started_{false};
    bool paused_{false};
    bool halted_{false};
    bool risk_block_{false};
    bool stopping_{false};
    bool strategy_on_{true};
    bool link_ok_{true};
    uint32_t cfg_ver_{1};
    char note_[64]{};

    uint64_t seen_[64]{};
    int seen_n_{0};
    int seen_head_{0};
    Bucket manual_bucket_[60]{};
    int64_t reports_{0};
    int64_t reports_rej_{0};
};

}  // namespace pitlane
