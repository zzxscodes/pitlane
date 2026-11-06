#pragma once

#include "pitlane/numeric.hpp"

#include <cstdint>

namespace pitlane {

// Buckets grow with the magnitude of the sample. The value stored for a bucket is its lower edge.
// Percentile 0 is the smallest sample. Percentile 100 is the largest. Ranks in between use the bucket edge.
// At most 64 paced notes are recorded. A longer stall returns -1 after those 64.

struct MagHist {
    static constexpr int kHalfBits = 3;
    static constexpr int kHalf = 1 << kHalfBits;
    static constexpr int kSub = kHalf << 1;
    static constexpr int kMags = 16;
    static constexpr int kLen = (kMags + 1) * kHalf;

    uint64_t bin[kLen]{};
    uint64_t total{0};
    uint64_t lo{0};
    uint64_t hi{0};

    static int index_of(uint64_t value) noexcept {
        const unsigned long long x = value | static_cast<unsigned long long>(kSub - 1);
        const int ceiling = 64 - __builtin_clzll(x);
        int mag = ceiling - (kHalfBits + 1);
        if (mag < 0) mag = 0;
        const int sub = static_cast<int>(value >> mag);
        return ((mag + 1) << kHalfBits) + (sub - kHalf);
    }

    static uint64_t low_at(int index) noexcept {
        int mag = (index >> kHalfBits) - 1;
        int sub = (index & (kHalf - 1)) + kHalf;
        if (mag < 0) {
            sub -= kHalf;
            mag = 0;
        }
        return static_cast<uint64_t>(sub) << mag;
    }

    bool add(uint64_t value) noexcept {
        const int index = index_of(value);
        if (index < 0 || index >= kLen) return false;
        if (bin[index] == UINT64_MAX || total == UINT64_MAX) return false;
        ++bin[index];
        if (total == 0 || value < lo) lo = value;
        if (total == 0 || value > hi) hi = value;
        ++total;
        return true;
    }

    bool pct(int numer, int denom, uint64_t& out) const noexcept {
        if (total == 0 || denom <= 0 || numer < 0 || numer > denom) return false;
        if (numer == 0) {
            out = lo;
            return true;
        }
        if (numer == denom) {
            out = hi;
            return true;
        }
        const Wide prod = mul_wide(total, static_cast<uint64_t>(numer));
        uint64_t need = 0;
        if (!div_wide(prod, static_cast<uint64_t>(denom), need)) return false;
        const Wide back = mul_wide(need, static_cast<uint64_t>(denom));
        if (cmp_wide(prod, back) > 0) ++need;
        if (need == 0) need = 1;
        if (need > total) need = total;
        uint64_t run = 0;
        int i = 0;
        const int lim = kLen - (kLen % 4);
        for (; i < lim; i += 4) {
            const uint64_t block = bin[i] + bin[i + 1] + bin[i + 2] + bin[i + 3];
            if (run + block >= need) {
                for (int j = 0; j < 4; ++j) {
                    run += bin[i + j];
                    if (run >= need) {
                        out = low_at(i + j);
                        return true;
                    }
                }
            } else {
                run += block;
            }
        }
        for (; i < kLen; ++i) {
            run += bin[i];
            if (run >= need) {
                out = low_at(i);
                return true;
            }
        }
        out = hi;
        return true;
    }
};

struct LatReport {
    uint64_t n{0};
    uint64_t min{0};
    uint64_t max{0};
    uint64_t p50{0};
    uint64_t p99{0};
    uint64_t p999{0};
};

inline bool mag_report(const MagHist& hist, LatReport& out) noexcept {
    if (hist.total == 0) return false;
    out.n = hist.total;
    out.min = hist.lo;
    out.max = hist.hi;
    if (!hist.pct(1, 2, out.p50)) return false;
    if (!hist.pct(99, 100, out.p99)) return false;
    if (!hist.pct(999, 1000, out.p999)) return false;
    return true;
}

template <class Note>
int note_paced(Note&& note, uint64_t took, uint64_t pace) {
    if (!note(took)) return 0;
    int n = 1;
    if (pace == 0 || took <= pace) return n;
    for (uint64_t left = took - pace; left >= pace; left -= pace) {
        if (n >= 64) return -1;
        if (!note(left)) return n;
        ++n;
    }
    return n;
}

}  // namespace pitlane
