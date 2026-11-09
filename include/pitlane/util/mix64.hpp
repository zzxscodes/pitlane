#pragma once

#include <cstdint>

namespace pitlane {

// SplitMix64. The sequence is fixed for a seed. This is not a cryptographic generator.

struct Mix64 {
    uint64_t state{0};

    uint64_t next() noexcept {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    uint64_t below(uint64_t n) noexcept {
        const uint64_t draw = next();
        if (n <= 1) return 0;
        const uint64_t reject = static_cast<uint64_t>(-n) % n;
        uint64_t x = draw;
        while (x < reject) x = next();
        return x % n;
    }

    bool span(int64_t lo, int64_t hi, int64_t& out) noexcept {
        if (hi < lo) return false;
        const uint64_t width = static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo);
        if (width == UINT64_MAX) {
            out = static_cast<int64_t>(next());
            return true;
        }
        out = static_cast<int64_t>(static_cast<uint64_t>(lo) + below(width + 1));
        return true;
    }

    bool bytes(uint8_t* dest, int n) noexcept {
        if (n < 0 || (n > 0 && !dest)) return false;
        int at = 0;
        while (at < n) {
            uint64_t x = next();
            for (int k = 0; k < 8 && at < n; ++k, ++at) {
                dest[at] = static_cast<uint8_t>(x);
                x >>= 8;
            }
        }
        return true;
    }
};

}  // namespace pitlane
