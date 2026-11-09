#pragma once

#include <cstddef>

namespace pitlane {

// Reads one byte on each cache line so the pages are resident before a timed run.
// The caller owns the bytes. The call does not allocate.

inline void touch_lines(const void* data, std::size_t bytes) noexcept {
    if (!data || bytes == 0) return;
    const auto* p = static_cast<const unsigned char*>(data);
    unsigned char acc = 0;
    for (std::size_t i = 0; i < bytes; i += 64) acc = static_cast<unsigned char>(acc + p[i]);
    acc = static_cast<unsigned char>(acc + p[bytes - 1]);
    asm volatile("" : "+r"(acc)::"memory");
}

}  // namespace pitlane
