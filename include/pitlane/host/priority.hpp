#pragma once

#include "pitlane/util/hint.hpp"

#include <cerrno>
#include <cstdint>

#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

namespace pitlane {

// Scheduling priority helpers for the calling thread or process.
// SCHED_FIFO needs CAP_SYS_NICE or root. A refused call returns false and leaves policy alone.

PITLANE_INLINE bool set_fifo(int priority) noexcept {
    const int max_p = ::sched_get_priority_max(SCHED_FIFO);
    const int min_p = ::sched_get_priority_min(SCHED_FIFO);
    if (unlikely(max_p < 0 || min_p < 0 || priority < min_p || priority > max_p)) return false;
    sched_param param{};
    param.sched_priority = priority;
    return ::sched_setscheduler(0, SCHED_FIFO, &param) == 0;
}

PITLANE_INLINE bool set_rr(int priority) noexcept {
    const int max_p = ::sched_get_priority_max(SCHED_RR);
    const int min_p = ::sched_get_priority_min(SCHED_RR);
    if (unlikely(max_p < 0 || min_p < 0 || priority < min_p || priority > max_p)) return false;
    sched_param param{};
    param.sched_priority = priority;
    return ::sched_setscheduler(0, SCHED_RR, &param) == 0;
}

PITLANE_INLINE bool set_other() noexcept {
    sched_param param{};
    param.sched_priority = 0;
    return ::sched_setscheduler(0, SCHED_OTHER, &param) == 0;
}

PITLANE_INLINE int get_policy() noexcept { return ::sched_getscheduler(0); }

PITLANE_INLINE bool set_nice(int nice) noexcept {
    errno = 0;
    const int got = ::setpriority(PRIO_PROCESS, 0, nice);
    return got == 0 && errno == 0;
}

PITLANE_INLINE int get_nice() noexcept {
    errno = 0;
    const int n = ::getpriority(PRIO_PROCESS, 0);
    if (errno != 0) return 0x7fffffff;
    return n;
}

}  // namespace pitlane
