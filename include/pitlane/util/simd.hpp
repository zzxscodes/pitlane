#pragma once

#include "pitlane/util/hint.hpp"

#include <cstddef>
#include <cstdint>

// Explicit SIMD helpers. Hot paths call these directly so a cold compiler still
// emits packed ops when the ISA is present. Scalar bodies cover every other build.
// AVX-512 stays out: the notes warn about frequency drops on Intel.

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__SSE4_2__)
#include <smmintrin.h>
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define PITLANE_IVDEP _Pragma("GCC ivdep")
#else
#define PITLANE_IVDEP
#endif

namespace pitlane {

#if defined(__AVX2__)
inline constexpr int kSimdI64 = 4;
#elif defined(__SSE2__)
inline constexpr int kSimdI64 = 2;
#else
inline constexpr int kSimdI64 = 1;
#endif
inline constexpr int kUnroll4 = 4;

PITLANE_INLINE bool aligned16(const void* p) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) & 15u) == 0;
}

PITLANE_INLINE bool aligned32(const void* p) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) & 31u) == 0;
}

// Clears n int64 words. Uses AVX2 or SSE2 packed stores when present.
PITLANE_HOT inline void zero_i64(int64_t* __restrict dst, int n) noexcept {
    if (unlikely(!dst || n <= 0)) return;
    int i = 0;
#if defined(__AVX2__)
    const __m256i z256 = _mm256_setzero_si256();
    if (aligned32(dst)) {
        int64_t* aligned = assume_aligned<32>(dst);
PITLANE_IVDEP
        for (; i + 3 < n; i += 4) {
            _mm256_store_si256(reinterpret_cast<__m256i*>(aligned + i), z256);
        }
    } else {
PITLANE_IVDEP
        for (; i + 3 < n; i += 4) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), z256);
        }
    }
#if defined(__AVX__)
    _mm256_zeroupper();
#endif
#endif
#if defined(__SSE2__)
    const __m128i z = _mm_setzero_si128();
    if (aligned16(dst)) {
        int64_t* aligned = assume_aligned<16>(dst);
PITLANE_IVDEP
        for (; i + 1 < n; i += 2) {
            _mm_store_si128(reinterpret_cast<__m128i*>(aligned + i), z);
        }
    } else {
PITLANE_IVDEP
        for (; i + 1 < n; i += 2) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), z);
        }
    }
#endif
PITLANE_IVDEP
    for (; i + 3 < n; i += kUnroll4) {
        dst[i] = 0;
        dst[i + 1] = 0;
        dst[i + 2] = 0;
        dst[i + 3] = 0;
    }
    for (; i < n; ++i) dst[i] = 0;
}

PITLANE_HOT inline void zero_u64(uint64_t* __restrict dst, int n) noexcept {
    zero_i64(reinterpret_cast<int64_t*>(dst), n);
}

// Non-temporal clear for a large write-once region. Requires 16-byte alignment.
// Issues sfence before the region may be read again.
PITLANE_HOT inline void zero_i64_stream(int64_t* __restrict dst, int n) noexcept {
    if (unlikely(!dst || n <= 0)) return;
#if defined(__SSE2__)
    if (aligned16(dst) && n >= 16) {
        const __m128i z = _mm_setzero_si128();
        int i = 0;
PITLANE_IVDEP
        for (; i + 1 < n; i += 2) {
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i), z);
        }
        _mm_sfence();
        for (; i < n; ++i) dst[i] = 0;
        return;
    }
#endif
    zero_i64(dst, n);
}

PITLANE_HOT inline void zero_bytes(void* __restrict dst, std::size_t n) noexcept {
    if (unlikely(!dst || n == 0)) return;
    auto* p = static_cast<unsigned char*>(dst);
    const std::size_t words = n / sizeof(int64_t);
    if (words > 0) {
        if (words >= 64 && aligned16(p)) {
            zero_i64_stream(reinterpret_cast<int64_t*>(p), static_cast<int>(words));
        } else {
            zero_i64(reinterpret_cast<int64_t*>(p), static_cast<int>(words));
        }
    }
    for (std::size_t i = words * sizeof(int64_t); i < n; ++i) p[i] = 0;
}

// Byte copy with packed loads and stores. Falls back to a 4-wide scalar peel.
PITLANE_HOT inline void copy_bytes(void* __restrict dst, const void* __restrict src, std::size_t n) noexcept {
    if (unlikely(!dst || !src || n == 0)) return;
    auto* d = static_cast<unsigned char*>(dst);
    const auto* s = static_cast<const unsigned char*>(src);
    std::size_t i = 0;
#if defined(__AVX2__)
PITLANE_IVDEP
    for (; i + 32 <= n; i += 32) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + i), v);
    }
#if defined(__AVX__)
    _mm256_zeroupper();
#endif
#endif
#if defined(__SSE2__)
PITLANE_IVDEP
    for (; i + 16 <= n; i += 16) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i), v);
    }
#endif
PITLANE_IVDEP
    for (; i + 4 <= n; i += 4) {
        d[i] = s[i];
        d[i + 1] = s[i + 1];
        d[i + 2] = s[i + 2];
        d[i + 3] = s[i + 3];
    }
    for (; i < n; ++i) d[i] = s[i];
}

PITLANE_HOT inline bool bytes_eq(const void* __restrict a, const void* __restrict b, std::size_t n) noexcept {
    if (a == b) return true;
    if (unlikely(!a || !b)) return false;
    const auto* x = static_cast<const unsigned char*>(a);
    const auto* y = static_cast<const unsigned char*>(b);
    std::size_t i = 0;
#if defined(__SSE2__)
    for (; i + 16 <= n; i += 16) {
        const __m128i u = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i));
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y + i));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(u, v)) != 0xffff) return false;
    }
#endif
    for (; i < n; ++i) {
        if (x[i] != y[i]) return false;
    }
    return true;
}

PITLANE_INLINE void cover_add(int64_t& got, int64_t need, int64_t px, int64_t qty, bool ok) noexcept {
    if (unlikely(!ok || px <= 0 || qty <= 0 || got >= need)) return;
    const int64_t room = need - got;
    got += qty < room ? qty : room;
}

#if defined(__SSE4_2__)
PITLANE_INLINE unsigned cover_mask2(const int64_t* __restrict px, int64_t limit, bool buy) noexcept {
    const __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(px));
    const __m128i lim = _mm_set1_epi64x(limit);
    const __m128i zero = _mm_setzero_si128();
    const __m128i pos = _mm_cmpgt_epi64(p, zero);
    __m128i in{};
    if (buy) {
        const __m128i gt = _mm_cmpgt_epi64(p, lim);
        in = _mm_andnot_si128(gt, _mm_set1_epi64x(-1));
    } else {
        const __m128i lim_gt = _mm_cmpgt_epi64(lim, p);
        in = _mm_andnot_si128(lim_gt, _mm_set1_epi64x(-1));
    }
    const __m128i ok = _mm_and_si128(pos, in);
    alignas(16) int64_t lane[2]{};
    _mm_store_si128(reinterpret_cast<__m128i*>(lane), ok);
    unsigned bits = 0;
    if (lane[0] != 0) bits |= 1u;
    if (lane[1] != 0) bits |= 2u;
    return bits;
}
#endif

#if defined(__AVX2__)
PITLANE_INLINE unsigned cover_mask4(const int64_t* __restrict px, int64_t limit, bool buy) noexcept {
    const __m256i p = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(px));
    const __m256i lim = _mm256_set1_epi64x(limit);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i pos = _mm256_cmpgt_epi64(p, zero);
    __m256i in{};
    if (buy) {
        const __m256i gt = _mm256_cmpgt_epi64(p, lim);
        in = _mm256_andnot_si256(gt, _mm256_set1_epi64x(-1));
    } else {
        const __m256i lim_gt = _mm256_cmpgt_epi64(lim, p);
        in = _mm256_andnot_si256(lim_gt, _mm256_set1_epi64x(-1));
    }
    const __m256i ok = _mm256_and_si256(pos, in);
    alignas(32) int64_t lane[4]{};
    _mm256_store_si256(reinterpret_cast<__m256i*>(lane), ok);
    _mm256_zeroupper();
    unsigned bits = 0;
    for (int i = 0; i < 4; ++i) {
        if (lane[i] != 0) bits |= 1u << i;
    }
    return bits;
}
#endif

PITLANE_HOT inline int64_t cover_qty_buy(const int64_t* __restrict px, const int64_t* __restrict qty, int n,
                                         int64_t limit, int64_t need) noexcept {
    if (unlikely(!px || !qty || n <= 0 || limit <= 0 || need <= 0)) return 0;
    int64_t got = 0;
    int i = 0;
#if defined(__AVX2__)
    for (; i + 3 < n && got < need; i += 4) {
        prefetch_r(px + i + 4);
        prefetch_r(qty + i + 4);
        const unsigned bits = cover_mask4(px + i, limit, true);
        if (bits & 1u) cover_add(got, need, px[i], qty[i], true);
        if (bits & 2u) cover_add(got, need, px[i + 1], qty[i + 1], true);
        if (bits & 4u) cover_add(got, need, px[i + 2], qty[i + 2], true);
        if (bits & 8u) cover_add(got, need, px[i + 3], qty[i + 3], true);
    }
#elif defined(__SSE4_2__)
    for (; i + 1 < n && got < need; i += 2) {
        prefetch_r(px + i + 2);
        prefetch_r(qty + i + 2);
        const unsigned bits = cover_mask2(px + i, limit, true);
        if (bits & 1u) cover_add(got, need, px[i], qty[i], true);
        if (bits & 2u) cover_add(got, need, px[i + 1], qty[i + 1], true);
    }
#endif
    for (; i + 3 < n && got < need; i += kUnroll4) {
        prefetch_r(px + i + 4);
        prefetch_r(qty + i + 4);
        cover_add(got, need, px[i], qty[i], px[i] <= limit);
        cover_add(got, need, px[i + 1], qty[i + 1], px[i + 1] <= limit);
        cover_add(got, need, px[i + 2], qty[i + 2], px[i + 2] <= limit);
        cover_add(got, need, px[i + 3], qty[i + 3], px[i + 3] <= limit);
    }
    for (; i < n && got < need; ++i) cover_add(got, need, px[i], qty[i], px[i] <= limit);
    return got;
}

PITLANE_HOT inline int64_t cover_qty_sell(const int64_t* __restrict px, const int64_t* __restrict qty, int n,
                                          int64_t limit, int64_t need) noexcept {
    if (unlikely(!px || !qty || n <= 0 || limit <= 0 || need <= 0)) return 0;
    int64_t got = 0;
    int i = 0;
#if defined(__AVX2__)
    for (; i + 3 < n && got < need; i += 4) {
        prefetch_r(px + i + 4);
        prefetch_r(qty + i + 4);
        const unsigned bits = cover_mask4(px + i, limit, false);
        if (bits & 1u) cover_add(got, need, px[i], qty[i], true);
        if (bits & 2u) cover_add(got, need, px[i + 1], qty[i + 1], true);
        if (bits & 4u) cover_add(got, need, px[i + 2], qty[i + 2], true);
        if (bits & 8u) cover_add(got, need, px[i + 3], qty[i + 3], true);
    }
#elif defined(__SSE4_2__)
    for (; i + 1 < n && got < need; i += 2) {
        prefetch_r(px + i + 2);
        prefetch_r(qty + i + 2);
        const unsigned bits = cover_mask2(px + i, limit, false);
        if (bits & 1u) cover_add(got, need, px[i], qty[i], true);
        if (bits & 2u) cover_add(got, need, px[i + 1], qty[i + 1], true);
    }
#endif
    for (; i + 3 < n && got < need; i += kUnroll4) {
        prefetch_r(px + i + 4);
        prefetch_r(qty + i + 4);
        cover_add(got, need, px[i], qty[i], px[i] >= limit);
        cover_add(got, need, px[i + 1], qty[i + 1], px[i + 1] >= limit);
        cover_add(got, need, px[i + 2], qty[i + 2], px[i + 2] >= limit);
        cover_add(got, need, px[i + 3], qty[i + 3], px[i + 3] >= limit);
    }
    for (; i < n && got < need; ++i) cover_add(got, need, px[i], qty[i], px[i] >= limit);
    return got;
}

}  // namespace pitlane
