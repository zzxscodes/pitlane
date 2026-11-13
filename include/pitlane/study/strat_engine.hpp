#pragma once

#include "pitlane/desk/engine.hpp"
#include "pitlane/desk/runtime.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/desk/wire.hpp"
#include "pitlane/study/strathook.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Local reject before an order is published (notes: size, side, id, time).
inline Why order_precheck(const Intent& in, uint64_t id_base = 0) noexcept {
    if (in.qty <= 0) return Why::Qty;
    if (!known_side(in.side)) return Why::Side;
    if (in.tag == 0) return Why::Missing;
    if (id_base != 0 && in.tag < id_base) return Why::Dup;
    if (in.stamp_us == 0) return Why::Stamp;
    if (in.act != static_cast<uint8_t>(Act::Enter) && in.act != static_cast<uint8_t>(Act::Pull)) return Why::Mode;
    return Why::None;
}

// Strategy role (cts::shmdb::Strategy) and its context.
// loop_task picks reports, quotes, rebuilds, market orders, prints, algo jobs,
// asset and position replies, signals, then packages. Inserts go out on the desk.

class StratEngine : public Engine {
public:
    unsigned reader{kStratReader};
    unsigned intent_mask{0};
    unsigned sub_mask{1ull << kMdReader};
    unsigned query_mask{1ull << kGateReader};
    unsigned algo_mask{1ull << kGateReader};
    uint64_t id_base{0};

    bool set_up() override { return Engine::set_up(); }

    void loop_task() override {
        if (!desk_) return;
        ring_take(desk_->reports, report_cur_, reader, false, [&](const Report& r) {
            on_report(r);
            on_order_rsp(r);
        });
        ring_take(desk_->quotes, quote_cur_, reader, false, [&](const Quote& q) { on_quote(q); });
        ring_take(desk_->rebuilds, rebuild_cur_, reader, false, [&](const Quote& q) { on_rebuild(q); });
        ring_take(desk_->mkt_ords, mkt_cur_, reader, false, [&](const MktNote& m) { on_mkt_order(m); });
        ring_take(desk_->prints, print_cur_, reader, false, [&](const TapePrint& p) {
            on_print(p);
            on_mkt_trade(p);
        });
        ring_take(desk_->algos, algo_cur_, reader, false, [&](const AlgoJob& job) {
            if (job.phase >= 5) on_algo_rsp(job);
            else on_algo(job);
        });
        ring_take(desk_->queries, query_cur_, reader, true, [&](const QueryNote& n) {
            if (n.last == 0) return;
            if (n.kind == 2) on_asset(n);
            else if (n.kind == 1) on_position(n);
        });
        ring_take(desk_->signals, sig_cur_, reader, false, [&](const SigNote& s) {
            on_signal(s);
            if (s.last) on_signal_batch(s);
        });
        ring_take(desk_->pack_reqs, pack_req_cur_, reader, false, [&](const PackageNote& n) { on_pack_req(n); });
        ring_take(desk_->pack_rsps, pack_rsp_cur_, reader, false, [&](const PackageNote& n) { on_pack_rsp(n); });
        housekeep();
    }

    PITLANE_HOT bool insert_order(const Intent& in) noexcept {
        if (!desk_ || order_precheck(in, id_base) != Why::None) return false;
        return desk_->intents.publish(in, intent_mask);
    }

    PITLANE_HOT bool cancel_order(uint64_t tag, uint32_t name, uint32_t acct, int64_t stamp_us) noexcept {
        if (!desk_ || tag == 0) return false;
        return strat_stage_pull(*desk_, tag, name, acct, stamp_us, intent_mask);
    }

    bool subscribe_code(const char* code, uint32_t name, SubKind kind = SubKind::Quote) noexcept {
        if (!desk_ || !code || !code[0]) return false;
        SubNote n{};
        n.name = name;
        n.engine = engine_id();
        n.kind = static_cast<uint8_t>(kind);
        slot_text(n.code, static_cast<int>(sizeof n.code), code);
        return desk_->subs.publish(n, sub_mask);
    }

    bool subscribe_signal(const char* topic, uint32_t name) noexcept {
        if (!desk_ || !topic || !topic[0]) return false;
        SubNote n{};
        n.name = name;
        n.engine = engine_id();
        n.kind = static_cast<uint8_t>(SubKind::Signal);
        slot_text(n.topic, static_cast<int>(sizeof n.topic), topic);
        slot_text(n.code, static_cast<int>(sizeof n.code), topic);
        return desk_->subs.publish(n, sub_mask);
    }

    bool drop_signals() noexcept {
        if (!desk_) return false;
        SubNote n{};
        n.engine = engine_id();
        n.kind = static_cast<uint8_t>(SubKind::DropSignals);
        return desk_->subs.publish(n, sub_mask);
    }

    bool query_asset(uint32_t acct, uint64_t req = 0) noexcept {
        QueryNote n{};
        n.kind = 2;
        n.acct = acct;
        n.req = req;
        n.engine = engine_id();
        return publish_query(n);
    }

    bool query_position(uint32_t acct, uint64_t req = 0) noexcept {
        QueryNote n{};
        n.kind = 1;
        n.acct = acct;
        n.req = req;
        n.engine = engine_id();
        return publish_query(n);
    }

    bool insert_algo(const AlgoJob& job) noexcept {
        if (!desk_) return false;
        return desk_->algos.publish(job, algo_mask);
    }

    bool pause_algo(uint64_t tag, uint32_t name) noexcept { return algo_phase(tag, name, 2); }
    bool resume_algo(uint64_t tag, uint32_t name) noexcept { return algo_phase(tag, name, 1); }

    bool cancel_algo(uint64_t tag, uint32_t name, uint32_t acct, int64_t stamp_us) noexcept {
        if (!desk_) return false;
        AlgoJob job{};
        job.tag = tag;
        job.name = name;
        job.acct = acct;
        job.born_us = stamp_us;
        job.phase = 4;
        return desk_->algos.publish(job, algo_mask);
    }

    void on_control(const ControlNote& n) override {
        if (n.kind == 5) on_new_date(n.stamp_us);
        else if (n.kind == 6) on_new_param(n.text);
        else if (n.kind == 7) on_param_done(n.text);
        else if (n.kind == 8) on_date_done(n.stamp_us);
        else if (n.kind == 9) on_backtest_done();
    }

    StratEngine() { control_reader = kStratReader; }

protected:
    virtual void on_quote(const Quote&) {}
    virtual void on_report(const Report&) {}
    virtual void on_order_rsp(const Report&) {}
    virtual void on_print(const TapePrint&) {}
    virtual void on_mkt_trade(const TapePrint&) {}
    virtual void on_mkt_order(const MktNote&) {}
    virtual void on_rebuild(const Quote&) {}
    virtual void on_algo(const AlgoJob&) {}
    virtual void on_algo_rsp(const AlgoJob&) {}
    virtual void on_asset(const QueryNote&) {}
    virtual void on_position(const QueryNote&) {}
    virtual void on_signal(const SigNote&) {}
    virtual void on_signal_batch(const SigNote&) {}
    virtual void on_pack_req(const PackageNote&) {}
    virtual void on_pack_rsp(const PackageNote&) {}
    virtual void on_new_date(int64_t) {}
    virtual void on_new_param(const char*) {}
    virtual void on_param_done(const char*) {}
    virtual void on_date_done(int64_t) {}
    virtual void on_backtest_done() {}

private:
    bool publish_query(const QueryNote& n) noexcept {
        if (!desk_) return false;
        return desk_->queries.publish(n, query_mask);
    }

    bool algo_phase(uint64_t tag, uint32_t name, uint8_t phase) noexcept {
        if (!desk_ || tag == 0) return false;
        AlgoJob job{};
        job.tag = tag;
        job.name = name;
        job.phase = phase;
        return desk_->algos.publish(job, algo_mask);
    }

    uint64_t quote_cur_{0};
    uint64_t report_cur_{0};
    uint64_t print_cur_{0};
    uint64_t algo_cur_{0};
    uint64_t query_cur_{0};
    uint64_t rebuild_cur_{0};
    uint64_t mkt_cur_{0};
    uint64_t sig_cur_{0};
    uint64_t pack_req_cur_{0};
    uint64_t pack_rsp_cur_{0};
};

} // namespace pitlane
