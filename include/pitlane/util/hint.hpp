#pragma once

#include <cstddef>
#include <cstdint>

// Branch layout, hot and cold placement, cache-line constants, and prefetch.
// These tips rearrange code and warm lines. They do not change capacities or matching.

#if defined(__GNUC__) || defined(__clang__)
#define PITLANE_LIKELY(x) (__builtin_expect(!!(x), 1))
#define PITLANE_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#define PITLANE_INLINE inline __attribute__((always_inline))
#define PITLANE_NOINLINE __attribute__((noinline))
#define PITLANE_HOT __attribute__((hot))
#define PITLANE_COLD __attribute__((cold))
#define PITLANE_FLATTEN __attribute__((flatten))
#define PITLANE_ALIGN(n) __attribute__((aligned(n)))
#else
#define PITLANE_LIKELY(x) (!!(x))
#define PITLANE_UNLIKELY(x) (!!(x))
#define PITLANE_INLINE inline
#define PITLANE_NOINLINE
#define PITLANE_HOT
#define PITLANE_COLD
#define PITLANE_FLATTEN
#define PITLANE_ALIGN(n)
#endif

namespace pitlane {

inline constexpr std::size_t kCacheLine = 64;

PITLANE_INLINE bool likely(bool cond) noexcept { return PITLANE_LIKELY(cond); }
PITLANE_INLINE bool unlikely(bool cond) noexcept { return PITLANE_UNLIKELY(cond); }

// Read-intent prefetch. locality 3 keeps the line as long as the hardware allows.
PITLANE_INLINE void prefetch_r(const void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, 0, 3);
#else
    (void)p;
#endif
}

// Write-intent prefetch before a store into a cold or contended line.
PITLANE_INLINE void prefetch_w(void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, 1, 3);
#else
    (void)p;
#endif
}

// Walks a range one cache line at a time with a write-intent prefetch.
PITLANE_INLINE void prefetch_lines(void* data, std::size_t bytes) noexcept {
    if (unlikely(!data || bytes == 0)) return;
    auto* p = static_cast<unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; i += kCacheLine) prefetch_w(p + i);
    prefetch_w(p + (bytes - 1));
}

PITLANE_INLINE void prefetch_lines(const void* data, std::size_t bytes) noexcept {
    if (unlikely(!data || bytes == 0)) return;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; i += kCacheLine) prefetch_r(p + i);
    prefetch_r(p + (bytes - 1));
}

}  // namespace pitlane
