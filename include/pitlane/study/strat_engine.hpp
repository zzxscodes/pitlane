#pragma once

#include "pitlane/desk/engine.hpp"
#include "pitlane/desk/runtime.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/desk/wire.hpp"
#include "pitlane/study/strathook.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Strategy engine role (cts::shmdb::Strategy).
// Drains Desk rings and stages orders via intents (InsertOrder role).

class StratEngine : public Engine {
public:
    unsigned reader{kLoopReader};
    unsigned intent_mask{0};
    uint64_t quote_cur{0};
    uint64_t report_cur{0};
    uint64_t print_cur{0};
    uint64_t algo_cur{0};

    bool set_up() override { return Engine::set_up(); }

    void loop_task() override {
        if (!desk_) return;
        Quote q{};
        bool fresh = false;
        while (desk_->quotes.at(quote_cur, reader, q, &fresh)) {
            if (!fresh) break;
            desk_->quotes.ack(quote_cur, reader);
            ++quote_cur;
            on_quote(q);
        }
        Report r{};
        while (desk_->reports.at(report_cur, reader, r, &fresh)) {
            if (!fresh) break;
            desk_->reports.ack(report_cur, reader);
            ++report_cur;
            on_report(r);
        }
        TapePrint p{};
        while (desk_->prints.at(print_cur, reader, p, &fresh)) {
            if (!fresh) break;
            desk_->prints.ack(print_cur, reader);
            ++print_cur;
            on_print(p);
        }
        AlgoJob job{};
        while (desk_->algos.at(algo_cur, reader, job, &fresh)) {
            if (!fresh) break;
            desk_->algos.ack(algo_cur, reader);
            ++algo_cur;
            on_algo(job);
        }
    }

    PITLANE_HOT bool insert_order(const Intent& in) noexcept {
        if (!desk_) return false;
        return desk_->intents.publish(in, intent_mask);
    }

    PITLANE_HOT bool cancel_order(uint64_t tag, uint32_t name, uint32_t acct, int64_t stamp_us) noexcept {
        if (!desk_) return false;
        return strat_stage_pull(*desk_, tag, name, acct, stamp_us, intent_mask);
    }

protected:
    virtual void on_quote(const Quote&) {}
    virtual void on_report(const Report&) {}
    virtual void on_print(const TapePrint&) {}
    virtual void on_algo(const AlgoJob&) {}
};

} // namespace pitlane
