#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace pitlane {

// Eight slots in a bucket. The 16-bit signature is compared before the key.
// Signature 0 means empty. A computed 0 is stored as 1.
// One writer. A failed insert leaves the previous keys in place.

template <int Buckets, int Keys>
class SigBook {
public:
    static_assert(Buckets >= 2 && Buckets <= 256 && (Buckets & (Buckets - 1)) == 0, "buckets");
    static_assert(Keys >= 1 && Keys <= 1024, "keys");

    int live() const noexcept { return live_; }

    int find(const uint8_t* key) const noexcept {
        if (!key) return 0;
        const uint32_t h1 = mix(key, 2166136261u);
        const uint32_t h2 = mix(key, 2166136261u ^ 0x9e3779b9u);
        const uint16_t sig = sig16(h1);
        const int a = match(h1 & mask(), sig, key);
        if (a > 0) return a;
        return match(h2 & mask(), sig, key);
    }

    bool put(const uint8_t* key) noexcept {
        if (!key || find(key) > 0) return find(key) > 0;
        int slot = -1;
        for (int i = 0; i < Keys; ++i) {
            if (!used_[i]) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return false;
        std::memcpy(key_[slot], key, 16);
        const uint32_t h1 = mix(key, 2166136261u);
        const uint32_t h2 = mix(key, 2166136261u ^ 0x9e3779b9u);
        const uint16_t sig = sig16(h1);
        const uint32_t primary = h1 & mask();
        const uint32_t alt = h2 & mask();
        if (place_empty(primary, static_cast<uint32_t>(slot + 1), sig) ||
            (alt != primary && place_empty(alt, static_cast<uint32_t>(slot + 1), sig)) ||
            kick(primary, static_cast<uint32_t>(slot + 1), sig, h1)) {
            used_[slot] = 1;
            ++live_;
            return true;
        }
        std::memset(key_[slot], 0, 16);
        return false;
    }

private:
    static constexpr uint32_t mask() noexcept { return static_cast<uint32_t>(Buckets - 1); }

    struct Bucket {
        std::atomic<uint16_t> sig[8]{};
        std::atomic<uint32_t> ix[8]{};
    };

    struct Undo {
        uint32_t bucket;
        int slot;
        uint16_t sig;
        uint32_t ix;
    };

    static uint32_t mix(const uint8_t* key, uint32_t seed) noexcept {
        uint32_t h = seed;
        for (int i = 0; i < 16; ++i) {
            h ^= key[i];
            h *= 16777619u;
        }
        return h;
    }

    static uint16_t sig16(uint32_t h) noexcept {
        const uint16_t s = static_cast<uint16_t>(h >> 16);
        return s == 0 ? 1 : s;
    }

    int match(uint32_t bucket, uint16_t sig, const uint8_t* key) const noexcept {
        const Bucket& bk = tab_[bucket];
        for (int i = 0; i < 8; ++i) {
            if (bk.sig[i].load(std::memory_order_acquire) != sig) continue;
            const uint32_t ix = bk.ix[i].load(std::memory_order_acquire);
            if (ix == 0 || ix > static_cast<uint32_t>(Keys)) continue;
            if (std::memcmp(key_[ix - 1], key, 16) == 0) return static_cast<int>(ix);
        }
        return 0;
    }

    bool place_empty(uint32_t bucket, uint32_t ix, uint16_t sig) noexcept {
        Bucket& bk = tab_[bucket];
        for (int i = 0; i < 8; ++i) {
            if (bk.sig[i].load(std::memory_order_relaxed) != 0) continue;
            bk.ix[i].store(ix, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            bk.sig[i].store(sig, std::memory_order_release);
            return true;
        }
        return false;
    }

    bool kick(uint32_t bucket, uint32_t ix, uint16_t sig, uint32_t h1) noexcept {
        Undo undo[8];
        int nu = 0;
        for (int step = 0; step < 8; ++step) {
            Bucket& bk = tab_[bucket];
            int empty = -1;
            for (int i = 0; i < 8; ++i) {
                if (bk.sig[i].load(std::memory_order_relaxed) == 0) {
                    empty = i;
                    break;
                }
            }
            const int slot = empty >= 0 ? empty : static_cast<int>((h1 + static_cast<uint32_t>(step)) & 7u);
            const uint16_t old_s = bk.sig[slot].load(std::memory_order_relaxed);
            const uint32_t old_ix = bk.ix[slot].load(std::memory_order_relaxed);
            undo[nu++] = Undo{bucket, slot, old_s, old_ix};
            if (empty >= 0) {
                bk.ix[slot].store(ix, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                bk.sig[slot].store(sig, std::memory_order_release);
                return true;
            }
            bk.sig[slot].store(0, std::memory_order_release);
            bk.ix[slot].store(ix, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            bk.sig[slot].store(sig, std::memory_order_release);
            if (old_ix == 0 || old_ix > static_cast<uint32_t>(Keys)) break;
            const uint8_t* old_key = key_[old_ix - 1];
            const uint32_t primary = mix(old_key, 2166136261u) & mask();
            const uint32_t alt = mix(old_key, 2166136261u ^ 0x9e3779b9u) & mask();
            bucket = bucket == primary ? alt : primary;
            ix = old_ix;
            sig = old_s;
            h1 ^= 0x9e3779b9u;
        }
        for (int i = nu - 1; i >= 0; --i) {
            Bucket& bk = tab_[undo[i].bucket];
            bk.sig[undo[i].slot].store(0, std::memory_order_relaxed);
            bk.ix[undo[i].slot].store(undo[i].ix, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            bk.sig[undo[i].slot].store(undo[i].sig, std::memory_order_release);
        }
        return false;
    }

    Bucket tab_[Buckets]{};
    uint8_t key_[Keys][16]{};
    uint8_t used_[Keys]{};
    int live_{0};
};

}  // namespace pitlane
