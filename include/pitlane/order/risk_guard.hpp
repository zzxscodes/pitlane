#pragma once

// Post-trade risk guard. The pretrade limits live in Rms; this layer watches
// the account and the connection at 1 Hz and latches a block or a halt.

#include "pitlane/ops/policy.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

struct RiskInputs {
    int64_t equity{0};
    int64_t peak_equity{0};
    int64_t daily_pnl{0};
    int64_t order_count_1s{0};
    int64_t rejects{0};
    int64_t orders{0};
    int64_t last_tick_us{0};
    int64_t now_us{0};
    bool md_up{true};
    bool td_up{true};
};

enum class RiskAction : uint8_t { Ok = 0, Warn = 1, BlockNew = 2, Halt = 3 };

enum : uint8_t {
    kRiskNone = 0,
    kRiskDailyLoss = 1,
    kRiskDrawdown = 2,
    kRiskRejectRate = 3,
    kRiskMdDown = 4,
    kRiskTdDown = 5,
    kRiskStaleTick = 6,
    kRiskOrderRate = 7,
};

struct RiskEvent {
    int64_t stamp_us{0};
    uint8_t level{0};
    uint8_t code{0};
    char text[48]{};
};

class RiskGuard {
public:
    void set(const OpsPolicy& p) noexcept { pol_ = p; }

    RiskAction evaluate(const RiskInputs& in, RiskEvent& ev) noexcept {
        if (halted_) {
            ev = last_;
            return RiskAction::Halt;
        }
        if (in.peak_equity > 0 && in.equity >= 0) {
            const int64_t drop = in.peak_equity - in.equity;
            if (pol_.max_drawdown_ppm > 0 && drop > 0) {
                const int64_t ppm = drop > INT64_MAX / 1000000 ? INT64_MAX : drop * 1000000 / in.peak_equity;
                if (ppm >= pol_.max_drawdown_ppm) return latch(ev, kRiskDrawdown, RiskAction::Halt, in.now_us,
                                                              "max drawdown");
            }
        }
        if (pol_.max_daily_loss > 0 && in.daily_pnl <= -pol_.max_daily_loss) {
            return latch(ev, kRiskDailyLoss, RiskAction::Halt, in.now_us, "daily loss");
        }
        if (pol_.max_reject_ppm > 0 && in.orders >= 20) {
            const int64_t ppm = in.rejects * 1000000 / in.orders;
            if (ppm >= pol_.max_reject_ppm) return latch(ev, kRiskRejectRate, RiskAction::BlockNew, in.now_us,
                                                         "reject rate");
            if (ppm * 2 >= pol_.max_reject_ppm) {
                return make(ev, kRiskRejectRate, RiskAction::Warn, in.now_us, "reject rate high");
            }
        }
        if (!in.md_up) return latch(ev, kRiskMdDown, RiskAction::BlockNew, in.now_us, "market data down");
        if (!in.td_up) return latch(ev, kRiskTdDown, RiskAction::BlockNew, in.now_us, "trading channel down");
        if (pol_.stale_tick_us > 0 && in.last_tick_us > 0 && in.now_us - in.last_tick_us > pol_.stale_tick_us) {
            return latch(ev, kRiskStaleTick, RiskAction::BlockNew, in.now_us, "stale tick");
        }
        blocked_ = false;
        ev = RiskEvent{};
        return RiskAction::Ok;
    }

    // Clears the block and the halt latch. A halted guard stays halted until this call.
    void resume() noexcept {
        halted_ = false;
        blocked_ = false;
        last_ = RiskEvent{};
    }

    bool blocked() const noexcept { return blocked_ || halted_; }

    bool halted() const noexcept { return halted_; }

    const RiskEvent& last() const noexcept { return last_; }

private:
    static void fill(RiskEvent& ev, uint8_t code, RiskAction action, int64_t now_us, const char* text) noexcept {
        ev = RiskEvent{};
        ev.stamp_us = now_us;
        ev.level = static_cast<uint8_t>(action);
        ev.code = code;
        std::snprintf(ev.text, sizeof ev.text, "%s", text);
    }

    RiskAction make(RiskEvent& ev, uint8_t code, RiskAction action, int64_t now_us, const char* text) noexcept {
        fill(ev, code, action, now_us, text);
        if (action != RiskAction::Warn) blocked_ = true;
        return action;
    }

    RiskAction latch(RiskEvent& ev, uint8_t code, RiskAction action, int64_t now_us, const char* text) noexcept {
        fill(ev, code, action, now_us, text);
        blocked_ = true;
        if (action == RiskAction::Halt) halted_ = true;
        last_ = ev;
        return action;
    }

    OpsPolicy pol_{};
    bool blocked_{false};
    bool halted_{false};
    RiskEvent last_{};
};

}  // namespace pitlane
