#pragma once

#include <cstdint>

namespace pitlane {

// Half-open range of indices into a row array ordered by stamp.
struct TimeSpan {
    int begin{0};
    int end{0};

    int count() const noexcept { return end - begin; }
    bool empty() const noexcept { return end <= begin; }
};

// Rows must already be ordered by stamp. Both ends are binary searched, so a
// research pass pays O(log n) rather than walking the whole tape. The range is
// [from, to): a row stamped exactly at to is excluded.
template <class T, class Stamp>
inline TimeSpan span_by_time(const T* rows, int n, int64_t from, int64_t to, Stamp&& stamp) noexcept {
    if (!rows || n <= 0 || to <= from) return TimeSpan{};
    int lo = 0;
    int hi = n;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (stamp(rows[mid]) < from) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    const int begin = lo;
    hi = n;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (stamp(rows[mid]) < to) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return TimeSpan{begin, lo};
}

// Newest row with stamp <= t, or nullptr before the first row. This is the
// lookup a caller wants when it needs the quote that was live at a decision
// time rather than the first quote after it.
template <class T, class Stamp>
inline const T* latest_by_time(const T* rows, int n, int64_t t, Stamp&& stamp) noexcept {
    if (!rows || n <= 0) return nullptr;
    int lo = 0;
    int hi = n;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (stamp(rows[mid]) <= t) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo == 0 ? nullptr : &rows[lo - 1];
}

}  // namespace pitlane
