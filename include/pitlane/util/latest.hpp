#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <type_traits>

namespace pitlane {

// Keeps only the newest payload per key. A lagging reader drains dirty slots once
// instead of replaying a stale backlog. Fail-closed when Cap distinct keys are full.

template <class T, int Cap>
class LatestKeep {
public:
    static_assert(Cap >= 1 && Cap <= 4096, "cap");
    static_assert(std::is_trivially_copyable<T>::value, "body");

    int live() const noexcept { return live_; }
    int dirty() const noexcept { return dirty_; }

    PITLANE_HOT bool put(uint32_t key, const T& item) noexcept {
        for (int i = 0; i < live_; ++i) {
            if (slot_[i].key != key) continue;
            slot_[i].body = item;
            if (!slot_[i].dirty) {
                slot_[i].dirty = 1;
                ++dirty_;
            }
            return true;
        }
        if (unlikely(live_ >= Cap)) return false;
        slot_[live_].key = key;
        slot_[live_].body = item;
        slot_[live_].dirty = 1;
        ++live_;
        ++dirty_;
        return true;
    }

    // Copies dirty slots into out (and optional keys). Clears dirty flags. Returns count.
    PITLANE_HOT int drain(T* out, int cap, uint32_t* keys = nullptr) noexcept {
        if (unlikely(!out || cap <= 0 || dirty_ == 0)) return 0;
        int n = 0;
        for (int i = 0; i < live_ && n < cap; ++i) {
            if (!slot_[i].dirty) continue;
            out[n] = slot_[i].body;
            if (keys) keys[n] = slot_[i].key;
            slot_[i].dirty = 0;
            ++n;
        }
        dirty_ -= n;
        if (dirty_ < 0) dirty_ = 0;
        return n;
    }

    void clear() noexcept {
        live_ = 0;
        dirty_ = 0;
    }

private:
    struct Slot {
        uint32_t key{0};
        uint8_t dirty{0};
        T body{};
    };

    Slot slot_[Cap]{};
    int live_{0};
    int dirty_{0};
};

}  // namespace pitlane
