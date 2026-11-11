#pragma once

#include "pitlane/util/hint.hpp"
#include "pitlane/util/simd.hpp"

#include <cstddef>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace pitlane {

// Prefetches and loads each cache line so the pages are resident before a timed run.
// The packed load is written in the source so a weak auto-vectorizer still warms the line.
// The caller owns the bytes. The call does not allocate.

PITLANE_HOT inline void touch_lines(const void* data, std::size_t bytes) noexcept {
    if (unlikely(!data || bytes == 0)) return;
    const auto* p = static_cast<const unsigned char*>(data);
    unsigned char acc = 0;
    for (std::size_t i = 0; i < bytes; i += kCacheLine) {
        prefetch_r(p + i);
#if defined(__SSE2__)
        if (i + 16 <= bytes) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
            acc = static_cast<unsigned char>(acc + static_cast<unsigned char>(_mm_cvtsi128_si32(v)));
        } else {
            acc = static_cast<unsigned char>(acc + p[i]);
        }
#else
        acc = static_cast<unsigned char>(acc + p[i]);
#endif
    }
    prefetch_r(p + (bytes - 1));
    acc = static_cast<unsigned char>(acc + p[bytes - 1]);
    asm volatile("" : "+r"(acc)::"memory");
}

}  // namespace pitlane
