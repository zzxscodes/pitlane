#pragma once

#include "pitlane/util/hint.hpp"

#include <sys/mman.h>

namespace pitlane {

// Process-wide page locking. Needs CAP_IPC_LOCK or sufficient RLIMIT_MEMLOCK.
// Mapping-level mlock stays on MapOpt / ArenaOpt; this pins the whole address space.

inline bool mem_lock_all() noexcept {
    return ::mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
}

inline bool mem_unlock_all() noexcept { return ::munlockall() == 0; }

}  // namespace pitlane
