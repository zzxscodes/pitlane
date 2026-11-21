#pragma once

#include <cstdint>

namespace pitlane {

// Address of one core's slot. The caller owns the block.
// The stride is a multiple of 64 bytes. The library does not allocate it.

inline bool core_slot(void* base, uint32_t core, uint32_t cores, uint32_t stride, void*& out) noexcept {
    if (!base || cores == 0 || core >= cores || stride < 64u || (stride & 63u) != 0u) return false;
    if (core != 0 && stride > UINT64_MAX / core) return false;
    out = static_cast<char*>(base) + static_cast<uint64_t>(core) * stride;
    return true;
}

}  // namespace pitlane
