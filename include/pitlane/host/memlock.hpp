#pragma once

#include "pitlane/util/hint.hpp"

#include <cstddef>
#include <sys/mman.h>
#include <sys/resource.h>

namespace pitlane {

// Process-wide and range page locking. Needs CAP_IPC_LOCK or sufficient RLIMIT_MEMLOCK.
// Library mappings pin with MapOpt::lock / ArenaOpt::lock (mlock on the range).
// mem_lock_all pins the whole address space for the calling process.

// Raises the soft RLIMIT_MEMLOCK to the hard limit. Best-effort before mlockall.
inline bool mem_raise_limit() noexcept {
    rlimit lim{};
    if (::getrlimit(RLIMIT_MEMLOCK, &lim) != 0) return false;
    if (lim.rlim_cur == lim.rlim_max) return true;
    lim.rlim_cur = lim.rlim_max;
    return ::setrlimit(RLIMIT_MEMLOCK, &lim) == 0;
}

inline bool mem_lock_all() noexcept {
    (void)mem_raise_limit();
    return ::mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
}

inline bool mem_unlock_all() noexcept { return ::munlockall() == 0; }

PITLANE_INLINE bool mem_lock_range(void* addr, std::size_t bytes) noexcept {
    if (unlikely(!addr || bytes == 0)) return false;
    return ::mlock(addr, bytes) == 0;
}

PITLANE_INLINE bool mem_unlock_range(void* addr, std::size_t bytes) noexcept {
    if (unlikely(!addr || bytes == 0)) return false;
    return ::munlock(addr, bytes) == 0;
}

}  // namespace pitlane
