#pragma once

#include "pitlane/desk/engine.hpp"
#include "pitlane/desk/runtime.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Trader-client role (cts::broker::TraderClient): send/cancel to a counter.
struct TdClient {
    virtual ~TdClient() = default;
    virtual int insert(const Intent& in) = 0;
    virtual int cancel(uint64_t tag, uint32_t name) = 0;
    virtual bool ready() const noexcept = 0;
};

// Trader engine role (cts::broker::Trader).
// Drains Desk intents (reader kLoopReader by default) and publishes reports.

class TdEngine : public Engine {
public:
    TdClient* client{nullptr};
    unsigned intent_reader{kLoopReader};
    unsigned report_mask{0};
    uint64_t intent_cur{0};

    bool set_up() override { return Engine::set_up() && client != nullptr; }

    void loop_task() override {
        if (!desk_ || !client || !client->ready()) return;
        Intent in{};
        bool fresh = false;
        while (desk_->intents.at(intent_cur, intent_reader, in, &fresh)) {
            if (!fresh) break;
            desk_->intents.ack(intent_cur, intent_reader);
            ++intent_cur;
            if (in.act == static_cast<uint8_t>(Act::Pull)) {
                (void)client->cancel(in.tag, in.name);
            } else if (in.act == static_cast<uint8_t>(Act::Enter)) {
                (void)client->insert(in);
            }
        }
    }

protected:
    PITLANE_HOT bool write_report(const Report& r) noexcept {
        if (!desk_) return false;
        return desk_->reports.publish(r, report_mask);
    }
};

} // namespace pitlane
