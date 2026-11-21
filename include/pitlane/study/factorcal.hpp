#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Fixed calendar factor series. No heap/map/string on the write path.
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

    // Writes a slot the caller already resolved. Unlike write(), gaps are
    // decided by slot distance, so an irregular calendar (lunch break, night
    // session) is not mistaken for a missing sample.
    bool write_slot(int idx, int64_t t, int64_t value, int64_t gap_v) noexcept {
        if (idx < 0 || idx >= Depth) return false;
        if (last_write != kNever && idx <= last_write) return false;
        if (last_write != kNever && idx > last_write + 1) {
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

private:
    FactorCell cell_[Depth]{};
};

// Ordered trading-calendar tags. A regular grid maps time in constant time;
// an irregular grid falls back to a branchless binary search. Tags are copied
// in, so the map owns no pointer and the caller can rebuild it at startup.
template <int Tags>
class CalendarMap {
public:
    static_assert(Tags >= 1 && Tags <= 65536, "tags");

    bool reset(const int64_t* tags, int n) noexcept {
        if ((n > 0 && !tags) || n < 0 || n > Tags) return false;
        for (int i = 1; i < n; ++i) {
            if (tags[i] <= tags[i - 1]) return false;
        }
        for (int i = 0; i < n; ++i) tag_[i] = tags[i];
        n_ = n;
        regular_ = 1;
        step_ = 0;
        if (n >= 2) {
            step_ = tag_[1] - tag_[0];
            for (int i = 2; i < n; ++i) {
                if (tag_[i] - tag_[i - 1] != step_) {
                    regular_ = 0;
                    break;
                }
            }
        }
        return true;
    }

    bool add(int64_t t) noexcept {
        if (n_ >= Tags) return false;
        if (n_ > 0 && t <= tag_[n_ - 1]) return false;
        if (n_ == 1) {
            step_ = t - tag_[0];
        } else if (n_ > 1 && t - tag_[n_ - 1] != step_) {
            regular_ = 0;
        }
        tag_[n_++] = t;
        return true;
    }

    int size() const noexcept { return n_; }
    bool regular() const noexcept { return regular_ != 0; }
    int64_t step() const noexcept { return step_; }
    int64_t tag(int idx) const noexcept { return idx >= 0 && idx < n_ ? tag_[idx] : 0; }

    // Exact tag lookup. Returns -1 when the time is off the grid.
    PITLANE_HOT int index_of(int64_t t) const noexcept {
        if (n_ == 0) return -1;
        if (regular_ && step_ > 0) {
            const int64_t d = t - tag_[0];
            if (d < 0 || d % step_ != 0) return -1;
            const int64_t i = d / step_;
            return i < n_ ? static_cast<int>(i) : -1;
        }
        if (n_ == 1) return t == tag_[0] ? 0 : -1;
        int lo = 0;
        int hi = n_;
        while (lo < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (tag_[mid] < t) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo < n_ && tag_[lo] == t ? lo : -1;
    }

    // Greatest tag <= t, or -1 before the first tag.
    int floor_index(int64_t t) const noexcept {
        if (n_ == 0 || t < tag_[0]) return -1;
        if (t >= tag_[n_ - 1]) return n_ - 1;
        if (regular_ && step_ > 0) return static_cast<int>((t - tag_[0]) / step_);
        int lo = 0;
        int hi = n_;
        while (lo < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (tag_[mid] <= t) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo - 1;
    }

    // Least tag >= t, or -1 past the last tag.
    int ceil_index(int64_t t) const noexcept {
        if (n_ == 0 || t > tag_[n_ - 1]) return -1;
        if (t <= tag_[0]) return 0;
        if (regular_ && step_ > 0) return static_cast<int>((t - tag_[0] + step_ - 1) / step_);
        int lo = 0;
        int hi = n_;
        while (lo < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (tag_[mid] < t) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

private:
    int64_t tag_[Tags]{};
    int n_{0};
    int64_t step_{0};
    uint8_t regular_{1};
};

// Named factor ring on one instrument. Fixed keys, fail-full.
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

// Calendar-aligned named factors. The map resolves time, the ring resolves the
// name, and the series fills skipped slots. Writes with an unknown or rewound
// time fail closed.
template <int Names, int Tags, int Depth>
class CalendarFactorStore {
public:
    bool reset(const int64_t* tags, int n) noexcept {
        ring_ = NamedFactorRing<Names, Depth>{};
        return calendar_.reset(tags, n);
    }

    bool add_tag(int64_t t) noexcept { return calendar_.add(t); }

    bool write(const char* key, int64_t t, int64_t value, int64_t gap_v) noexcept {
        const int idx = calendar_.index_of(t);
        if (idx < 0) return false;
        auto* slot = ring_.open(key);
        return slot && slot->series.write_slot(idx, t, value, gap_v);
    }

    // Gap value is the last accepted reading, or zero before the first sample.
    bool write_carry(const char* key, int64_t t, int64_t value) noexcept {
        const int idx = calendar_.index_of(t);
        if (idx < 0) return false;
        auto* slot = ring_.open(key);
        if (!slot) return false;
        int64_t gap = 0;
        if (slot->series.last_write != FactorSeries<Depth>::kNever) {
            gap = slot->series.at(static_cast<int>(slot->series.last_write))->value;
        }
        return slot->series.write_slot(idx, t, value, gap);
    }

    const FactorCell* at(const char* key, int64_t t) const noexcept {
        const int idx = calendar_.index_of(t);
        if (idx < 0) return nullptr;
        const auto* slot = ring_.find(key);
        return slot ? slot->series.at(idx) : nullptr;
    }

    const FactorCell* latest(const char* key) const noexcept {
        const auto* slot = ring_.find(key);
        if (!slot || slot->series.last_write == FactorSeries<Depth>::kNever) return nullptr;
        return slot->series.at(static_cast<int>(slot->series.last_write));
    }

    int64_t last_time(const char* key) const noexcept {
        const auto* slot = ring_.find(key);
        return slot ? slot->series.last_time : 0;
    }

    const CalendarMap<Tags>& calendar() const noexcept { return calendar_; }

private:
    CalendarMap<Tags> calendar_{};
    NamedFactorRing<Names, Depth> ring_{};
};

}  // namespace pitlane
