#pragma once

// Trading-session arithmetic in minutes. `Span` lives in `calendar.hpp` and
// uses the same clock: `begin_min == end_min` is empty, `end_min < begin_min`
// wraps over midnight.
//
// All lengths are integer tradable minutes. Elapsed time before minute `m` is
// the overlap of every span with `[0, m)`; a wrapping span contributes
// `[begin, 1440)` plus `[0, end)`. `session_add_min` advances through open
// spans, skips breaks, and reports clock-day rollovers in `day_shift`.
//
// These helpers are for scheduling (remaining session, deadline arithmetic),
// not for the per-tick path. Nothing allocates and nothing loops over wall
// clock time.

#include "pitlane/book/calendar.hpp"

#include <cstdint>

namespace pitlane {

inline constexpr int kMinutesPerDay = 1440;

inline int session_norm_min(int64_t minute) noexcept {
    int64_t m = minute % kMinutesPerDay;
    if (m < 0) m += kMinutesPerDay;
    return static_cast<int>(m);
}

inline int session_span_min(const Span& s) noexcept {
    const int b = session_norm_min(s.begin_min);
    const int e = session_norm_min(s.end_min);
    if (b == e) return 0;
    return e > b ? e - b : kMinutesPerDay - b + e;
}

inline int session_total_min(const Span* spans, int n) noexcept {
    if (!spans || n <= 0) return 0;
    int total = 0;
    for (int i = 0; i < n; ++i) {
        const int len = session_span_min(spans[i]);
        if (len > kMinutesPerDay - total) return kMinutesPerDay;
        total += len;
    }
    return total;
}

inline bool session_open_at(const Span* spans, int n, int minute) noexcept {
    if (!spans || n <= 0) return false;
    const int m = session_norm_min(minute);
    for (int i = 0; i < n; ++i) {
        const int b = session_norm_min(spans[i].begin_min);
        const int e = session_norm_min(spans[i].end_min);
        if (b == e) continue;
        if (b < e) {
            if (m >= b && m < e) return true;
        } else if (m >= b || m < e) {
            return true;
        }
    }
    return false;
}

// Tradable minutes strictly before `minute` on the clock day.
inline int session_elapsed_min(const Span* spans, int n, int64_t minute) noexcept {
    if (!spans || n <= 0) return 0;
    const int m = session_norm_min(minute);
    int64_t sum = 0;
    for (int i = 0; i < n; ++i) {
        const int b = session_norm_min(spans[i].begin_min);
        const int e = session_norm_min(spans[i].end_min);
        if (b == e) continue;
        int64_t overlap = 0;
        if (b < e) {
            if (m <= b) {
                overlap = 0;
            } else if (m >= e) {
                overlap = e - b;
            } else {
                overlap = m - b;
            }
        } else {
            const int64_t evening = m > b ? m - b : 0;
            const int64_t morning = m < e ? m : e;
            overlap = evening + morning;
            const int64_t len = kMinutesPerDay - b + e;
            if (overlap > len) overlap = len;
        }
        sum += overlap;
        if (sum > kMinutesPerDay) sum = kMinutesPerDay;
    }
    return static_cast<int>(sum);
}

inline int session_remaining_min(const Span* spans, int n, int64_t minute) noexcept {
    const int total = session_total_min(spans, n);
    int left = total - session_elapsed_min(spans, n, minute);
    return left > 0 ? left : 0;
}

inline int session_next_open_min(const Span* spans, int n, int minute) noexcept {
    if (!spans || n <= 0) return 0;
    const int m = session_norm_min(minute);
    int best = kMinutesPerDay;
    for (int i = 0; i < n; ++i) {
        const int b = session_norm_min(spans[i].begin_min);
        const int e = session_norm_min(spans[i].end_min);
        if (b == e) continue;
        bool open = false;
        if (b < e) {
            open = m >= b && m < e;
        } else {
            open = m >= b || m < e;
        }
        if (open) return 0;
        int d = b - m;
        if (d <= 0) d += kMinutesPerDay;
        if (d < best) best = d;
    }
    return best == kMinutesPerDay ? 0 : best;
}

inline int session_next_close_min(const Span* spans, int n, int minute) noexcept {
    if (!spans || n <= 0) return 0;
    const int m = session_norm_min(minute);
    int best = kMinutesPerDay * 2;
    for (int i = 0; i < n; ++i) {
        const int b = session_norm_min(spans[i].begin_min);
        const int e = session_norm_min(spans[i].end_min);
        if (b == e) continue;
        const int len = b < e ? e - b : kMinutesPerDay - b + e;
        int d = 0;
        if (b < e) {
            d = (m >= b && m < e) ? e - m : ((b - m + kMinutesPerDay) % kMinutesPerDay) + len;
        } else {
            d = (m >= b || m < e) ? (e > m ? e - m : kMinutesPerDay - m + e) : ((b - m + kMinutesPerDay) % kMinutesPerDay) + len;
        }
        if (d < best) best = d;
    }
    return best >= kMinutesPerDay * 2 ? 0 : best;
}

// Backward distance to the previous close boundary. A minute sitting exactly on
// a close yields 0; the caller consumes the minute before the boundary itself.
inline int ses_prev_close_min(const Span* spans, int n, int minute) noexcept {
    if (!spans || n <= 0) return 0;
    const int m = session_norm_min(minute);
    int best = kMinutesPerDay;
    for (int i = 0; i < n; ++i) {
        const int b = session_norm_min(spans[i].begin_min);
        const int e = session_norm_min(spans[i].end_min);
        if (b == e) continue;
        int d = m - e;
        if (d < 0) d += kMinutesPerDay;
        if (d < best) best = d;
    }
    return best == kMinutesPerDay ? 0 : best;
}

inline int session_sub_min(const Span* spans, int n, int minute, int64_t delta, int* day_shift) noexcept;

inline int session_add_min(const Span* spans, int n, int minute, int64_t delta, int* day_shift) noexcept {
    const int start = session_norm_min(minute);
    if (day_shift) *day_shift = 0;
    const int total = session_total_min(spans, n);
    if (!spans || n <= 0 || total <= 0 || delta == 0) return start;
    if (delta < 0) {
        const int64_t back = delta == INT64_MIN ? INT64_MAX : -delta;
        return session_sub_min(spans, n, start, back, day_shift);
    }

    int64_t shift = delta / total;
    int64_t rem = delta % total;
    int m = start;
    while (rem > 0) {
        const int idx = span_index(m, spans, n);
        if (idx < 0) {
            const int d = session_next_open_min(spans, n, m);
            if (d <= 0) break;
            m += d;
            if (m >= kMinutesPerDay) {
                m -= kMinutesPerDay;
                ++shift;
            }
            continue;
        }
        const int b = session_norm_min(spans[idx].begin_min);
        const int e = session_norm_min(spans[idx].end_min);
        int to_end = 0;
        if (b < e) {
            to_end = e - m;
        } else {
            to_end = m >= b ? kMinutesPerDay - m + e : e - m;
        }
        if (to_end <= 0) break;
        if (rem <= to_end) {
            m += static_cast<int>(rem);
            rem = 0;
            if (m >= kMinutesPerDay) {
                m -= kMinutesPerDay;
                ++shift;
            }
            break;
        }
        rem -= to_end;
        m += to_end;
        if (m >= kMinutesPerDay) {
            m -= kMinutesPerDay;
            ++shift;
        }
    }
    if (shift > INT32_MAX) shift = INT32_MAX;
    if (shift < INT32_MIN) shift = INT32_MIN;
    if (day_shift) *day_shift = static_cast<int>(shift);
    return m;
}

inline int session_sub_min(const Span* spans, int n, int minute, int64_t delta, int* day_shift) noexcept {
    const int start = session_norm_min(minute);
    if (day_shift) *day_shift = 0;
    const int total = session_total_min(spans, n);
    if (!spans || n <= 0 || total <= 0 || delta == 0) return start;
    if (delta < 0) {
        const int64_t fwd = delta == INT64_MIN ? INT64_MAX : -delta;
        return session_add_min(spans, n, start, fwd, day_shift);
    }

    int64_t shift = -(delta / total);
    int64_t rem = delta % total;
    int m = start;
    while (rem > 0) {
        const int idx = span_index(m, spans, n);
        if (idx >= 0) {
            const int b = session_norm_min(spans[idx].begin_min);
            const int e = session_norm_min(spans[idx].end_min);
            int to_begin = 0;
            if (b < e) {
                to_begin = m - b;
            } else {
                to_begin = m >= b ? m - b : m + kMinutesPerDay - b;
            }
            if (rem <= to_begin) {
                int64_t back = static_cast<int64_t>(m) - rem;
                if (back < 0) {
                    back += kMinutesPerDay;
                    --shift;
                }
                m = static_cast<int>(back);
                rem = 0;
                break;
            }
            rem -= to_begin;
            m = b;
        }
        const int d = ses_prev_close_min(spans, n, m);
        m -= d;
        if (m < 0) {
            m += kMinutesPerDay;
            --shift;
        }
        if (rem > 0) {
            --m;
            if (m < 0) {
                m += kMinutesPerDay;
                --shift;
            }
            --rem;
        }
    }
    if (shift > INT32_MAX) shift = INT32_MAX;
    if (shift < INT32_MIN) shift = INT32_MIN;
    if (day_shift) *day_shift = static_cast<int>(shift);
    return m;
}

}  // namespace pitlane
