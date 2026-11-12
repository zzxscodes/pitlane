#pragma once

#include "pitlane/desk/region.hpp"
#include "pitlane/desk/runtime.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/desk/wire.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Local strategy façade (Strategy::On* / Context insert role).
// Fn-ptrs only; sits on DeskLoop. Hot path no alloc.

struct StratHook {
    void (*on_quote)(void* user, const Quote& q){nullptr};
    void (*on_report)(void* user, const Report& r){nullptr};
    void (*on_print)(void* user, const TapePrint& p){nullptr};
    void (*on_pulse)(void* user, const Pulse& p){nullptr};
    void (*on_timer)(void* user, uint64_t id, int64_t now_us){nullptr};
    void* user{nullptr};
};

inline LoopHooks strat_bind_loop(const StratHook& s) noexcept {
    LoopHooks h{};
    h.on_quote = s.on_quote;
    h.on_report = s.on_report;
    h.on_print = s.on_print;
    h.on_pulse = s.on_pulse;
    h.on_timer = s.on_timer;
    h.user = s.user;
    return h;
}

PITLANE_HOT inline bool strat_stage_enter(Desk& desk, const Intent& in, unsigned mask = 0) noexcept {
    return desk.intents.publish(in, mask);
}

PITLANE_HOT inline bool strat_stage_pull(Desk& desk, uint64_t tag, uint32_t name, uint32_t acct, int64_t stamp_us,
                                         unsigned mask = 0) noexcept {
    Intent in{};
    in.tag = tag;
    in.name = name;
    in.acct = acct;
    in.stamp_us = stamp_us;
    in.act = static_cast<uint8_t>(Act::Pull);
    return desk.intents.publish(in, mask);
}

} // namespace pitlane
