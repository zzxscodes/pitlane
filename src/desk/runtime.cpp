#include "pitlane/desk/runtime.hpp"

namespace pitlane {

template <typename T, uint32_t Cap>
int DeskLoop::drain(FanRing<T, Cap>& ring, uint64_t& cursor, void (*fn)(void*, const T&), void* user) noexcept {
    int n = 0;
    const uint64_t end = ring.tail();
    while (cursor < end) {
        T item{};
        bool fresh = false;
        if (!ring.at(cursor, kLoopReader, item, &fresh)) {
            cursor = end;
            break;
        }
        if (fresh) {
            ring.ack(cursor, kLoopReader);
            if (fn) fn(user, item);
            ++n;
        }
        ++cursor;
    }
    return n;
}

bool DeskLoop::add_timer(uint64_t id, int64_t when_us, int64_t every_us) noexcept {
    if (id == 0) return false;
    for (int i = 0; i < kTimers; ++i) {
        if (timer_[i].live && timer_[i].id == id) {
            timer_[i].when_us = when_us;
            timer_[i].every_us = every_us;
            return true;
        }
    }
    for (int i = 0; i < kTimers; ++i) {
        if (!timer_[i].live) {
            timer_[i] = Timer{id, when_us, every_us, 1};
            return true;
        }
    }
    return false;
}

int DeskLoop::beat(Desk& desk, int64_t now_us, const LoopHooks& hooks) noexcept {
    if (stop_) return 0;
    desk.clock_us.store(now_us, std::memory_order_release);
    int n = 0;
    n += drain(desk.quotes, quote_cur_, hooks.on_quote, hooks.user);
    n += drain(desk.reports, report_cur_, hooks.on_report, hooks.user);
    n += drain(desk.pulses, pulse_cur_, hooks.on_pulse, hooks.user);
    n += drain(desk.prints, print_cur_, hooks.on_print, hooks.user);
    n += drain(desk.algos, algo_cur_, hooks.on_algo, hooks.user);
    n += drain(desk.queries, query_cur_, hooks.on_query, hooks.user);
    n += drain(desk.pack_reqs, pack_req_cur_, hooks.on_pack_req, hooks.user);
    n += drain(desk.pack_rsps, pack_rsp_cur_, hooks.on_pack_rsp, hooks.user);
    n += drain(desk.logs, log_cur_, hooks.on_log, hooks.user);

    const uint64_t end = desk.controls.tail();
    while (control_cur_ < end) {
        ControlNote note{};
        bool fresh = false;
        if (!desk.controls.at(control_cur_, kLoopReader, note, &fresh)) {
            control_cur_ = end;
            break;
        }
        if (fresh) {
            desk.controls.ack(control_cur_, kLoopReader);
            if (note.kind == 1) stop_ = 1;
            if (hooks.on_control) hooks.on_control(hooks.user, note);
            ++n;
        }
        ++control_cur_;
    }

    for (int i = 0; i < kTimers; ++i) {
        Timer& t = timer_[i];
        if (!t.live || t.when_us > now_us) continue;
        if (hooks.on_timer) hooks.on_timer(hooks.user, t.id, now_us);
        ++n;
        if (t.every_us > 0) t.when_us += t.every_us;
        else t.live = 0;
    }
    return n;
}

}  // namespace pitlane
