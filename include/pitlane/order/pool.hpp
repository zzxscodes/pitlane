#pragma once

#include <cstdint>
#include <type_traits>

namespace pitlane {

// Fixed slots for records the caller owns by id. A free stack reuses a slot.
// The id index is open addressing. A removed id is shifted back so the next lookup stays short.
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
            at_[i] = -1;
        }
    }

    int size() const noexcept { return Cap - nfree_; }

    bool take(uint64_t id, const T& body) noexcept {
        if (id == 0 || nfree_ == 0 || find_key(id) >= 0) return false;
        const int slot = free_[--nfree_];
        if (!insert_key(id, slot)) {
            ++nfree_;
            return false;
        }
        slot_[slot].id = id;
        slot_[slot].body = body;
        slot_[slot].live = 1;
        return true;
    }

    bool drop(uint64_t id) noexcept {
        const int k = find_key(id);
        if (k < 0) return false;
        const int slot = at_[k];
        erase_key(static_cast<uint32_t>(k));
        slot_[slot].live = 0;
        slot_[slot].id = 0;
        free_[nfree_++] = slot;
        return true;
    }

    T* find(uint64_t id) noexcept {
        const int k = find_key(id);
        if (k < 0) return nullptr;
        return &slot_[at_[k]].body;
    }

    const T* find(uint64_t id) const noexcept {
        const int k = find_key(id);
        if (k < 0) return nullptr;
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

    int find_key(uint64_t id) const noexcept {
        if (id == 0) return -1;
        const uint32_t mask = kIndex - 1;
        uint32_t i = static_cast<uint32_t>(mix(id) & mask);
        for (int n = 0; n < kIndex; ++n) {
            if (key_[i] == 0) return -1;
            if (key_[i] == id) return static_cast<int>(i);
            i = (i + 1u) & mask;
        }
        return -1;
    }

    bool insert_key(uint64_t id, int slot) noexcept {
        const uint32_t mask = kIndex - 1;
        uint32_t i = static_cast<uint32_t>(mix(id) & mask);
        for (int n = 0; n < kIndex; ++n) {
            if (key_[i] == 0) {
                key_[i] = id;
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
            if (key_[j] == 0) {
                key_[i] = 0;
                at_[i] = -1;
                return;
            }
            const uint32_t home = static_cast<uint32_t>(mix(key_[j]) & mask);
            const bool shift = (j > i) ? (home <= i || home > j) : (home <= i && home > j);
            if (!shift) {
                key_[i] = 0;
                at_[i] = -1;
                return;
            }
            key_[i] = key_[j];
            at_[i] = at_[j];
            i = j;
        }
    }

    Slot slot_[Cap]{};
    int free_[Cap]{};
    int nfree_{0};
    uint64_t key_[kIndex]{};
    int at_[kIndex]{};
};

}  // namespace pitlane
