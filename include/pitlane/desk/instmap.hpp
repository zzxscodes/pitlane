#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <cstring>

namespace pitlane {

// Fixed name table filled at startup. Hot path is a short linear scan.
struct InstMap {
    static constexpr int kMax = 32;
    char code[kMax][32]{};
    uint32_t name[kMax]{};
    int n{0};

    bool add(const char* c, uint32_t id) noexcept {
        if (!c || !c[0] || n >= kMax) return false;
        if (std::strlen(c) >= 32) return false;
        std::memcpy(code[n], c, std::strlen(c) + 1);
        name[n] = id;
        ++n;
        return true;
    }

    PITLANE_HOT uint32_t find(const char* c) const noexcept {
        if (!c) return 0;
        for (int i = 0; i < n; ++i) {
            if (std::strcmp(code[i], c) == 0) return name[i];
        }
        return 0;
    }
};

} // namespace pitlane
