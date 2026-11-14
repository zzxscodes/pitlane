#pragma once

#include "pitlane/desk/broker.hpp"
#include "pitlane/desk/engine.hpp"
#include "pitlane/desk/runtime.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Counter client the gateway implements.
// Sockets stay in the gateway. The library only calls these.
struct TdClient {
    virtual ~TdClient() = default;
    virtual int insert(const Intent& in) = 0;
    virtual int cancel(uint64_t tag, uint32_t name) = 0;
    virtual bool ready() const noexcept = 0;
    virtual int on_algo(const AlgoJob& job) {
        (void)job;
        return 0;
    }
    virtual int query_asset(const QueryNote& note) {
        (void)note;
        return 0;
    }
    virtual int query_position(const QueryNote& note) {
        (void)note;
        return 0;
    }
};

// Trader gateway.
// Loop: orders, asset queries, position queries, algo orders, then timeouts.
// risk_filter runs before insert. A hit is a denied report and the client is not called.

class TdEngine : public Engine {
public:
    TdClient* client{nullptr};
    BrokerWatch* watch{nullptr};
    unsigned intent_reader{kLoopReader};
    unsigned query_reader{kGateReader};
    unsigned algo_reader{kGateReader};
    unsigned report_mask{0};
    unsigned rsp_mask{1ull << kStratReader};
    int64_t ack_limit_us{60000000};
    int64_t life_limit_us{60000000};
    int64_t pull_limit_us{60000000};

    bool set_up() override { return Engine::set_up() && client != nullptr; }

    void loop_task() override {
        if (!desk_ || !client || !client->ready()) {
            housekeep();
            return;
        }
        process_orders();
        process_queries();
        process_algos();
        check_timeouts();
        housekeep();
    }

    virtual Why risk_filter(const Intent& in) {
        (void)in;
        return Why::None;
    }

    PITLANE_HOT bool write_report(const Report& r) noexcept {
        if (!desk_) return false;
        return desk_->reports.publish(r, report_mask);
    }

    bool write_asset_rsp(uint32_t acct, int64_t free_amt, uint64_t req = 0) noexcept {
        QueryNote n{};
        n.kind = 2;
        n.last = 1;
        n.acct = acct;
        n.req = req;
        n.cursor = free_amt;
        return write_query(n);
    }

    bool write_position_rsp(uint32_t acct, int64_t qty, uint64_t req = 0) noexcept {
        QueryNote n{};
        n.kind = 1;
        n.last = 1;
        n.acct = acct;
        n.req = req;
        n.cursor = qty;
        return write_query(n);
    }

    bool write_algo_rsp(AlgoJob job) noexcept {
        if (!desk_) return false;
        job.phase = 5;
        return desk_->algos.publish(job, rsp_mask);
    }

protected:
    bool write_query(const QueryNote& n) noexcept {
        if (!desk_) return false;
        return desk_->queries.publish(n, rsp_mask);
    }

    void process_orders() noexcept {
        ring_take(desk_->intents, intent_cur_, intent_reader, false, [&](const Intent& in) {
            if (in.act == static_cast<uint8_t>(Act::Pull)) {
                (void)client->cancel(in.tag, in.name);
                if (watch) (void)watch->mark_pull(in.tag, in.stamp_us);
                return;
            }
            if (in.act != static_cast<uint8_t>(Act::Enter)) return;
            const Why why = risk_filter(in);
            if (why != Why::None) {
                Report r{};
                r.tag = in.tag;
                r.name = in.name;
                r.px = in.px;
                r.left = in.qty;
                r.stamp_us = in.stamp_us;
                r.side = in.side;
                r.phase = static_cast<uint8_t>(Phase::Denied);
                r.why = static_cast<uint16_t>(why);
                (void)write_report(r);
                return;
            }
            if (watch) (void)watch->remember(in, in.stamp_us);
            (void)client->insert(in);
        });
    }

    void process_queries() noexcept {
        ring_take(desk_->queries, query_cur_, query_reader, true, [&](const QueryNote& n) {
            if (n.last != 0) return;
            if (n.kind == 2) (void)client->query_asset(n);
            else if (n.kind == 1) (void)client->query_position(n);
        });
    }

    void process_algos() noexcept {
        ring_take(desk_->algos, algo_cur_, algo_reader, true, [&](const AlgoJob& job) {
            if (job.phase >= 5) return;
            (void)client->on_algo(job);
        });
    }

    void check_timeouts() noexcept {
        if (!watch) return;
        const int64_t now = get_time_now();
        uint64_t tags[32];
        const int n = watch->expire(now, ack_limit_us, life_limit_us, pull_limit_us, tags, 32);
        for (int i = 0; i < n; ++i) {
            Report r{};
            r.tag = tags[i];
            r.stamp_us = now;
            r.phase = static_cast<uint8_t>(Phase::Expired);
            r.why = static_cast<uint16_t>(Why::Stamp);
            (void)write_report(r);
        }
    }

private:
    uint64_t intent_cur_{0};
    uint64_t query_cur_{0};
    uint64_t algo_cur_{0};
};

} // namespace pitlane
