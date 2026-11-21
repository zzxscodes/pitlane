#pragma once

#include "pitlane/util/hint.hpp"
#include "pitlane/util/simd.hpp"

#include <cstdint>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__SSE4_1__)
#include <smmintrin.h>
#endif

namespace pitlane {

// Dense rank over a bounded id space. bits[w] marks live ids; prefix[w] is the
// popcount of all earlier words. find returns the dense index, or -1.

template <uint32_t Words>
class RankBits {
public:
    static_assert(Words >= 1 && Words <= 4096u, "words");

    static constexpr uint32_t word_count() noexcept { return Words; }
    static constexpr uint32_t id_cap() noexcept { return Words * 64u; }

    void clear() noexcept {
        std::memset(bits_, 0, sizeof bits_);
        std::memset(prefix_, 0, sizeof prefix_);
        live_ = 0;
    }

    // Sets bits from ids, then rebuilds prefixes. Ids >= id_cap are ignored.
    bool build(const uint32_t* ids, uint32_t n) noexcept {
        clear();
        if (!ids && n > 0) return false;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t id = ids[i];
            if (id >= id_cap()) continue;
            bits_[id >> 6] |= 1ull << (id & 63u);
        }
        uint32_t run = 0;
        for (uint32_t w = 0; w < Words; ++w) {
            prefix_[w] = run;
            run += static_cast<uint32_t>(__builtin_popcountll(bits_[w]));
        }
        live_ = run;
        return true;
    }

    PITLANE_HOT int find(uint32_t id) const noexcept {
        const uint32_t w = id >> 6;
        if (unlikely(w >= Words)) return -1;
        const uint32_t b = id & 63u;
        const uint64_t word = bits_[w];
        if ((word & (1ull << b)) == 0) return -1;
        const uint64_t lower = ~0ull >> (63u - b);
        return static_cast<int>(prefix_[w] + static_cast<uint32_t>(__builtin_popcountll(word & lower)) - 1);
    }

    uint32_t live() const noexcept { return live_; }
    const uint64_t* bits() const noexcept { return bits_; }

private:
    uint64_t bits_[Words]{};
    uint32_t prefix_[Words]{};
    uint32_t live_{0};
};

// One cache line holds up to four static keys.
struct alignas(64) QuadLine {
    uint64_t key[4]{};
    uint32_t n{0};
    uint32_t pad{0};
};
static_assert(sizeof(QuadLine) == 64, "QuadLine one cache line");

// Static set: build reseeds until each line has at most four keys.
template <uint32_t Lines>
class QuadTab {
public:
    static_assert(Lines >= 1 && Lines <= 1024u && (Lines & (Lines - 1u)) == 0u, "lines pow2");

    void clear() noexcept {
        for (uint32_t i = 0; i < Lines; ++i) {
            line_[i].n = 0;
            line_[i].key[0] = line_[i].key[1] = line_[i].key[2] = line_[i].key[3] = 0;
        }
        seed_ = 0;
        live_ = 0;
    }

    bool seat(const uint64_t* keys, uint32_t n) noexcept {
        clear();
        if (!keys && n > 0) return false;
        if (n > Lines * 4u) return false;
        uint8_t cnt[Lines];
        for (uint64_t s = 1; s < 4096ull; ++s) {
            std::memset(cnt, 0, sizeof cnt);
            bool ok = true;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t b = static_cast<uint32_t>(mix(keys[i], s) & mask_);
                if (++cnt[b] > 4) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            for (uint32_t b = 0; b < Lines; ++b) line_[b].n = 0;
            for (uint32_t i = 0; i < n; ++i) {
                QuadLine& L = line_[static_cast<uint32_t>(mix(keys[i], s) & mask_)];
                L.key[L.n++] = keys[i];
            }
            seed_ = s;
            live_ = n;
            return true;
        }
        return false;
    }

    PITLANE_HOT int find(uint64_t k) const noexcept {
        const uint32_t b = static_cast<uint32_t>(mix(k, seed_) & mask_);
        const QuadLine& L = line_[b];
#if defined(__SSE4_1__)
        const __m128i want = _mm_set1_epi64x(static_cast<long long>(k));
        const __m128i lo = _mm_load_si128(reinterpret_cast<const __m128i*>(L.key));
        const __m128i hi = _mm_load_si128(reinterpret_cast<const __m128i*>(L.key + 2));
        int bits = _mm_movemask_pd(_mm_castsi128_pd(_mm_cmpeq_epi64(lo, want)));
        bits |= _mm_movemask_pd(_mm_castsi128_pd(_mm_cmpeq_epi64(hi, want))) << 2;
        for (uint32_t i = 0; i < L.n; ++i) {
            if (bits & (1 << static_cast<int>(i))) return static_cast<int>((b << 2) + i);
        }
#else
        for (uint32_t i = 0; i < L.n; ++i) {
            if (L.key[i] == k) return static_cast<int>((b << 2) + i);
        }
#endif
        return -1;
    }

    uint32_t live() const noexcept { return live_; }
    uint64_t seed() const noexcept { return seed_; }
    const QuadLine* lines() const noexcept { return line_; }

private:
    static constexpr uint32_t mask_ = Lines - 1u;

    static uint64_t mix(uint64_t k, uint64_t s) noexcept {
        k ^= s;
        k *= 0x9E3779B97F4A7C15ull;
        k ^= k >> 32;
        return k;
    }

    QuadLine line_[Lines]{};
    uint64_t seed_{0};
    uint32_t live_{0};
};

}  // namespace pitlane
