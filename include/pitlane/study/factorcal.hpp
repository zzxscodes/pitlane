#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Fixed calendar factor series (notes §22). No heap/map/string on the write path.
// Caller maps wall/calendar time → dense idx. last_write=-2 means never written.

struct FactorCell {
    int64_t value{0};
    uint8_t filled{0};  // 1 = gap-filled; 0 = real sample
    uint8_t on{0};
};

template <int Depth>
class FactorSeries {
public:
    static_assert(Depth >= 2 && Depth <= 4096, "depth");

    static constexpr int64_t kNever = -2;

    int64_t last_write{kNever};
    int64_t last_time{0};

    PITLANE_INLINE FactorCell* at(int idx) noexcept {
        if (idx < 0 || idx >= Depth) return nullptr;
        return &cell_[idx];
    }

    PITLANE_INLINE const FactorCell* at(int idx) const noexcept {
        if (idx < 0 || idx >= Depth) return nullptr;
        return &cell_[idx];
    }

    // Writes value at idx for time t. Gaps between last_write+1 and idx-1 get gap_v with filled=1.
    // Rejects idx <= last_write (except first write). Fail-closed on bad idx.
    template <class FillFn>
    bool write(int idx, int64_t t, int64_t value, int64_t time_step, FillFn&& fill) noexcept {
        if (idx < 0 || idx >= Depth) return false;
        if (last_write != kNever && idx <= last_write) return false;
        if (last_write != kNever && time_step > 0 && t - last_time != time_step) {
            int64_t gap_v = 0;
            fill(gap_v);
            for (int i = static_cast<int>(last_write) + 1; i < idx; ++i) {
                cell_[i].value = gap_v;
                cell_[i].filled = 1;
                cell_[i].on = 1;
            }
        }
        cell_[idx].value = value;
        cell_[idx].filled = 0;
        cell_[idx].on = 1;
        last_write = idx;
        last_time = t;
        return true;
    }

    bool write_plain(int idx, int64_t t, int64_t value, int64_t time_step, int64_t gap_v) noexcept {
        return write(idx, t, value, time_step, [gap_v](int64_t& out) noexcept { out = gap_v; });
    }

private:
    FactorCell cell_[Depth]{};
};

// Named factor ring on one instrument (notes §22 live mount). Fixed keys, fail-full.
template <int Names, int Depth>
class NamedFactorRing {
public:
    static_assert(Names >= 1 && Names <= 64, "names");
    static_assert(Depth >= 2 && Depth <= 1024, "depth");

    struct Slot {
        char key[32]{};
        uint8_t used{0};
        FactorSeries<Depth> series{};
    };

    Slot* find(const char* key) noexcept {
        const int i = index_of(key);
        return i < 0 ? nullptr : &slot_[i];
    }

    const Slot* find(const char* key) const noexcept {
        const int i = index_of(key);
        return i < 0 ? nullptr : &slot_[i];
    }

    Slot* open(const char* key) noexcept {
        if (!key || key[0] == 0) return nullptr;
        if (Slot* s = find(key)) return s;
        for (int i = 0; i < Names; ++i) {
            if (slot_[i].used) continue;
            if (!copy_key(slot_[i].key, key)) return nullptr;
            slot_[i].used = 1;
            slot_[i].series = FactorSeries<Depth>{};
            return &slot_[i];
        }
        return nullptr;
    }

private:
    static bool copy_key(char* dst, const char* src) noexcept {
        int n = 0;
        for (; src[n] && n < 31; ++n) dst[n] = src[n];
        if (src[n]) return false;
        dst[n] = 0;
        return true;
    }

    static bool key_eq(const char* a, const char* b) noexcept {
        if (!a || !b) return false;
        while (*a && *a == *b) {
            ++a;
            ++b;
        }
        return *a == 0 && *b == 0;
    }

    int index_of(const char* key) const noexcept {
        if (!key) return -1;
        for (int i = 0; i < Names; ++i) {
            if (slot_[i].used && key_eq(slot_[i].key, key)) return i;
        }
        return -1;
    }

    Slot slot_[Names]{};
};

}  // namespace pitlane
