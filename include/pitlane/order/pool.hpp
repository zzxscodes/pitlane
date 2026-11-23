#pragma once

#include "pitlane/util/hint.hpp"
#include "pitlane/util/simd.hpp"

#include <cstdint>
#include <type_traits>

namespace pitlane {

// Fixed slots for records the caller owns by id. A free stack reuses a slot.
// The id index is open addressing. A 16-bit signature sits ahead of the full key
// so a miss rejects before the 64-bit compare. A removed id is shifted back.
// Id 0 is empty. A duplicate id or a full pool leaves the pool unchanged.

template <class T, int Cap>
class SlotPool {
public:
    static_assert(Cap >= 2 && Cap <= 1024 && (Cap & (Cap - 1)) == 0, "slot pool");
    static_assert(std::is_trivially_copyable<T>::value, "slot body");

    SlotPool() noexcept {
        nfree_ = Cap;
        for (int i = 0; i < Cap; ++i) free_[i] = Cap - 1 - i;
        for (int i = 0; i < kIndex; ++i) {
            key_[i] = 0;
            sig_[i] = 0;
            at_[i] = -1;
        }
    }

    int size() const noexcept { return Cap - nfree_; }

    PITLANE_HOT bool take(uint64_t id, const T& body) noexcept {
        if (unlikely(id == 0 || nfree_ == 0 || find_key(id) >= 0)) return false;
        const int slot = free_[--nfree_];
        if (unlikely(!insert_key(id, slot))) {
            ++nfree_;
            return false;
        }
        slot_[slot].id = id;
        copy_bytes(&slot_[slot].body, &body, sizeof(T));
        slot_[slot].live = 1;
        return true;
    }

    bool drop(uint64_t id) noexcept {
        const int k = find_key(id);
        if (unlikely(k < 0)) return false;
        const int slot = at_[k];
        erase_key(static_cast<uint32_t>(k));
        slot_[slot].live = 0;
        slot_[slot].id = 0;
        free_[nfree_++] = slot;
        return true;
    }

    PITLANE_HOT T* find(uint64_t id) noexcept {
        const int k = find_key(id);
        if (unlikely(k < 0)) return nullptr;
        return &slot_[at_[k]].body;
    }

    PITLANE_HOT const T* find(uint64_t id) const noexcept {
        const int k = find_key(id);
        if (unlikely(k < 0)) return nullptr;
        return &slot_[at_[k]].body;
    }

private:
    static constexpr int kIndex = Cap * 2;

    struct Slot {
        uint64_t id{0};
        T body{};
        uint8_t live{0};
    };

    static uint64_t mix(uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    static uint16_t sig_of_mix(uint64_t m) noexcept {
        uint16_t s = static_cast<uint16_t>(m >> 48);
        return s == 0 ? static_cast<uint16_t>(1) : s;
    }

    PITLANE_INLINE int find_key(uint64_t id) const noexcept {
        if (unlikely(id == 0)) return -1;
        const uint32_t mask = kIndex - 1;
        const uint64_t m = mix(id);
        const uint16_t want = sig_of_mix(m);
        uint32_t i = static_cast<uint32_t>(m & mask);
        for (int n = 0; n < kIndex; ++n) {
            const uint16_t sg = sig_[i];
            if (likely(sg == 0)) return -1;
            if (likely(sg == want) && likely(key_[i] == id)) return static_cast<int>(i);
            i = (i + 1u) & mask;
        }
        return -1;
    }

    bool insert_key(uint64_t id, int slot) noexcept {
        const uint32_t mask = kIndex - 1;
        const uint64_t m = mix(id);
        const uint16_t want = sig_of_mix(m);
        uint32_t i = static_cast<uint32_t>(m & mask);
        for (int n = 0; n < kIndex; ++n) {
            if (sig_[i] == 0) {
                key_[i] = id;
                sig_[i] = want;
                at_[i] = slot;
                return true;
            }
            if (key_[i] == id) return false;
            i = (i + 1u) & mask;
        }
        return false;
    }

    void erase_key(uint32_t i) noexcept {
        const uint32_t mask = kIndex - 1;
        for (;;) {
            const uint32_t j = (i + 1u) & mask;
            if (sig_[j] == 0) {
                key_[i] = 0;
                sig_[i] = 0;
                at_[i] = -1;
                return;
            }
            const uint32_t home = static_cast<uint32_t>(mix(key_[j]) & mask);
            const bool shift = (j > i) ? (home <= i || home > j) : (home <= i && home > j);
            if (!shift) {
                key_[i] = 0;
                sig_[i] = 0;
                at_[i] = -1;
                return;
            }
            key_[i] = key_[j];
            sig_[i] = sig_[j];
            at_[i] = at_[j];
            i = j;
        }
    }

    Slot slot_[Cap]{};
    int free_[Cap]{};
    int nfree_{0};
    uint64_t key_[kIndex]{};
    uint16_t sig_[kIndex]{};
    int at_[kIndex]{};
};

}  // namespace pitlane
