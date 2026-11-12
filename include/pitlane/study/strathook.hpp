#pragma once

#include "pitlane/desk/region.hpp"
#include "pitlane/desk/runtime.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/desk/wire.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <cstring>

namespace pitlane {

// Local strategy façade (Strategy::On* / Context insert role).
// Fn-ptrs only; sits on DeskLoop. Hot path no alloc.

struct StratHook {
    void (*on_quote)(void* user, const Quote& q){nullptr};
    void (*on_report)(void* user, const Report& r){nullptr};
    void (*on_print)(void* user, const TapePrint& p){nullptr};
    void (*on_pulse)(void* user, const Pulse& p){nullptr};
    void (*on_algo)(void* user, const AlgoJob& job){nullptr};
    void (*on_pack_req)(void* user, const PackageNote& note){nullptr};
    void (*on_pack_rsp)(void* user, const PackageNote& note){nullptr};
    void (*on_log)(void* user, const LogNote& note){nullptr};
    void (*on_control)(void* user, const ControlNote& note){nullptr};
    void (*on_timer)(void* user, uint64_t id, int64_t now_us){nullptr};
    void* user{nullptr};
};

inline LoopHooks strat_bind_loop(const StratHook& s) noexcept {
    LoopHooks h{};
    h.on_quote = s.on_quote;
    h.on_report = s.on_report;
    h.on_print = s.on_print;
    h.on_pulse = s.on_pulse;
    h.on_algo = s.on_algo;
    h.on_pack_req = s.on_pack_req;
    h.on_pack_rsp = s.on_pack_rsp;
    h.on_log = s.on_log;
    h.on_control = s.on_control;
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

PITLANE_HOT inline bool strat_stage_algo(Desk& desk, const AlgoJob& job, unsigned mask = 0) noexcept {
    return desk.algos.publish(job, mask);
}

PITLANE_HOT inline bool strat_control(Desk& desk, uint8_t kind, uint32_t name = 0, int64_t stamp_us = 0,
                                      unsigned mask = 0) noexcept {
    ControlNote n{};
    n.kind = kind;
    n.name = name;
    n.stamp_us = stamp_us;
    return desk.controls.publish(n, mask);
}

PITLANE_HOT inline bool strat_pause(Desk& desk, uint32_t name = 0, int64_t stamp_us = 0, unsigned mask = 0) noexcept {
    return strat_control(desk, 3, name, stamp_us, mask);
}

PITLANE_HOT inline bool strat_resume(Desk& desk, uint32_t name = 0, int64_t stamp_us = 0, unsigned mask = 0) noexcept {
    return strat_control(desk, 4, name, stamp_us, mask);
}

PITLANE_HOT inline bool desk_pack_req(Desk& desk, const PackageNote& note, unsigned mask = 0) noexcept {
    return desk.pack_reqs.publish(note, mask);
}

PITLANE_HOT inline bool desk_pack_rsp(Desk& desk, const PackageNote& note, unsigned mask = 0) noexcept {
    return desk.pack_rsps.publish(note, mask);
}

PITLANE_HOT inline bool desk_log_publish(Desk& desk, int32_t level, int64_t stamp_us, const char* line,
                                         unsigned mask = 0) noexcept {
    LogNote n{};
    n.level = level;
    n.stamp_us = stamp_us;
    if (line) {
        const std::size_t lim = sizeof n.line - 1;
        std::size_t i = 0;
        for (; line[i] && i < lim; ++i) n.line[i] = line[i];
        n.line[i] = 0;
    }
    return desk.logs.publish(n, mask);
}

} // namespace pitlane
