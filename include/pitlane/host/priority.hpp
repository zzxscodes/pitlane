#pragma once

#include "pitlane/util/hint.hpp"

#include <cerrno>
#include <cstdint>

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

namespace pitlane {

// Scheduling priority helpers. SCHED_FIFO/RR need CAP_SYS_NICE or root.
// Process helpers use sched_setscheduler(0, …). Thread helpers use pthread_setschedparam
// on the calling thread (notes § realtime thread bind).

namespace detail {

PITLANE_INLINE bool set_policy(int policy, int priority) noexcept {
    const int max_p = ::sched_get_priority_max(policy);
    const int min_p = ::sched_get_priority_min(policy);
    if (unlikely(max_p < 0 || min_p < 0 || priority < min_p || priority > max_p)) return false;
    sched_param param{};
    param.sched_priority = priority;
    return ::sched_setscheduler(0, policy, &param) == 0;
}

PITLANE_INLINE bool set_thread_policy(int policy, int priority) noexcept {
    const int max_p = ::sched_get_priority_max(policy);
    const int min_p = ::sched_get_priority_min(policy);
    if (unlikely(max_p < 0 || min_p < 0 || priority < min_p || priority > max_p)) return false;
    sched_param param{};
    param.sched_priority = priority;
    return ::pthread_setschedparam(::pthread_self(), policy, &param) == 0;
}

}  // namespace detail

PITLANE_INLINE bool set_fifo(int priority) noexcept { return detail::set_policy(SCHED_FIFO, priority); }

PITLANE_INLINE bool set_rr(int priority) noexcept { return detail::set_policy(SCHED_RR, priority); }

PITLANE_INLINE bool set_fifo_thread(int priority) noexcept {
    return detail::set_thread_policy(SCHED_FIFO, priority);
}

PITLANE_INLINE bool set_rr_thread(int priority) noexcept {
    return detail::set_thread_policy(SCHED_RR, priority);
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
