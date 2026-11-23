#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <cstring>

namespace pitlane {

// Host owns capture and role assignment. Library only compares bytes / tests bits.

struct MacAddr {
    uint8_t b[6]{};
};

PITLANE_INLINE bool mac_eq(const MacAddr& a, const MacAddr& b) noexcept {
    return std::memcmp(a.b, b.b, 6) == 0;
}

template <int Cap = 3>
class MacAllow {
public:
    static_assert(Cap >= 1 && Cap <= 3, "mac whitelist <= 3");

    bool put(const MacAddr& mac) noexcept {
        for (int i = 0; i < n_; ++i) {
            if (mac_eq(row_[i], mac)) return true;
        }
        if (n_ >= Cap) return false;
        row_[n_++] = mac;
        return true;
    }

    bool has(const MacAddr& mac) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (mac_eq(row_[i], mac)) return true;
        }
        return false;
    }

    int size() const noexcept { return n_; }

private:
    MacAddr row_[Cap]{};
    int n_{0};
};

using RoleBits = uint32_t;

PITLANE_INLINE bool role_has(RoleBits bits, unsigned bit) noexcept {
    if (bit >= 32) return false;
    return (bits & (1u << bit)) != 0;
}

PITLANE_INLINE RoleBits role_set(RoleBits bits, unsigned bit) noexcept {
    if (bit >= 32) return bits;
    return bits | (1u << bit);
}

PITLANE_INLINE RoleBits role_clear(RoleBits bits, unsigned bit) noexcept {
    if (bit >= 32) return bits;
    return bits & ~(1u << bit);
}

}  // namespace pitlane
