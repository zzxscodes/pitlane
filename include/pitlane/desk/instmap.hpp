#pragma once

#include "pitlane/util/hint.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pitlane {

// Fixed name table filled at startup. The hot path is an open-addressed lookup,
// so a full instrument universe stays one probe away instead of a linear scan.
struct InstMap {
    static constexpr int kMax = 1024;
    static constexpr int kSlots = 2048;
    static_assert((kSlots & (kSlots - 1)) == 0, "slots");

    char code[kMax][32]{};
    uint32_t name[kMax]{};
    uint32_t slot[kSlots]{};
    int n{0};

    static uint32_t hash(const char* c) noexcept {
        uint32_t h = 2166136261u;
        while (*c) {
            h ^= static_cast<uint8_t>(*c++);
            h *= 16777619u;
        }
        return h;
    }

    static int index_of(uint32_t slot_value) noexcept { return static_cast<int>(slot_value) - 1; }

    bool add(const char* c, uint32_t id) noexcept {
        if (!c || !c[0] || n >= kMax) return false;
        const std::size_t len = std::strlen(c);
        if (len >= 32) return false;
        uint32_t i = hash(c) & static_cast<uint32_t>(kSlots - 1);
        while (slot[i] != 0) {
            if (std::strcmp(code[index_of(slot[i])], c) == 0) return true;
            i = (i + 1) & static_cast<uint32_t>(kSlots - 1);
        }
        std::memcpy(code[n], c, len + 1);
        name[n] = id;
        slot[i] = static_cast<uint32_t>(n) + 1;
        ++n;
        return true;
    }

    PITLANE_HOT uint32_t find(const char* c) const noexcept {
        if (!c || !c[0]) return 0;
        uint32_t i = hash(c) & static_cast<uint32_t>(kSlots - 1);
        while (slot[i] != 0) {
            const int idx = index_of(slot[i]);
            if (std::strcmp(code[idx], c) == 0) return name[idx];
            i = (i + 1) & static_cast<uint32_t>(kSlots - 1);
        }
        return 0;
    }
};

} // namespace pitlane
