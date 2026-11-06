#pragma once

#include <cstdint>

namespace pitlane {

// Nine session bands from the cash open through the cash close.
// Before 09:30 the lag is zero. Cuts are 10:00, 10:30, 11:00, 11:30, 13:30, 14:00, 14:30, 15:00.
// Index 8 covers 15:00 and later.

inline int session_band(int minute_of_day) noexcept {
    if (minute_of_day < 9 * 60 + 30) return -1;
    static constexpr int kCut[8] = {10 * 60, 10 * 60 + 30, 11 * 60, 11 * 60 + 30,
                                    13 * 60 + 30, 14 * 60, 14 * 60 + 30, 15 * 60};
    for (int i = 0; i < 8; ++i) {
        if (minute_of_day < kCut[i]) return i;
    }
    return 8;
}

inline int minute_from_us(int64_t stamp_us) noexcept {
    if (stamp_us < 0) return 0;
    int64_t sec = stamp_us / 1000000;
    if (sec >= 86400) sec %= 86400;
    return int(sec / 60);
}

struct LagTable {
    int64_t go_us[9]{};
    int64_t pull_us[9]{};
};

inline int64_t table_lag_us(const LagTable& t, int minute_of_day, bool pull) noexcept {
    const int b = session_band(minute_of_day);
    if (b < 0) return 0;
    return pull ? t.pull_us[b] : t.go_us[b];
}

inline int64_t ready_at(int64_t stamp_us, int64_t lag_us) noexcept {
    return stamp_us + lag_us;
}

// Overnight window: begin > end means the span crosses midnight.
inline bool inside_span(int hhmm, int begin, int end) noexcept {
    if (begin < end) return hhmm >= begin && hhmm < end;
    if (begin > end) return hhmm >= begin || hhmm < end;
    return false;
}

inline int hhmm_of(int hour, int minute) noexcept { return hour * 100 + minute; }

// Outside the session the previous target stays. Inside the session but outside the trade window it becomes zero.
inline int64_t stored_target(int64_t prev, int64_t summed, bool in_session, bool in_trade) noexcept {
    if (!in_session) return prev;
    if (!in_trade) return 0;
    return summed;
}

// Last price within one step of the limit-up or limit-down blocks a send.
inline bool send_room(bool in_session, bool slot_busy, int pulls_used, int pull_cap, int64_t last, int64_t up,
                      int64_t down, int64_t step) noexcept {
    if (!in_session || slot_busy) return false;
    if (pulls_used >= pull_cap) return false;
    if (step > 0 && up > down && (last >= up - step || last <= down + step)) return false;
    return true;
}

struct LagPoint {
    int64_t sent_ns{0};
    int64_t exch_ns{0};  // 0 means the request was rejected and never reached the book
    int64_t seen_ns{0};
};

inline int64_t lerp_i64(int64_t x, int64_t x0, int64_t y0, int64_t x1, int64_t y1) noexcept {
    if (x1 == x0) return y0;
    const double t = double(x - x0) / double(x1 - x0);
    return y0 + int64_t(t * double(y1 - y0));
}

// Go-lag is exchange time minus send time. A non-positive exchange time on either
// neighbour switches the interpolation onto the reject path and returns a negative delay.
inline int64_t go_delay(const LagPoint* s, int n, int& cursor, int64_t now) noexcept {
    if (!s || n <= 0) return 0;
    if (cursor < 0) cursor = 0;
    if (now < s[0].sent_ns) return s[0].exch_ns - s[0].sent_ns;
    while (cursor + 1 < n && s[cursor + 1].sent_ns <= now) ++cursor;
    if (cursor + 1 >= n) return s[n - 1].exch_ns - s[n - 1].sent_ns;
    const LagPoint& a = s[cursor];
    const LagPoint& b = s[cursor + 1];
    if (a.exch_ns <= 0 || b.exch_ns <= 0) {
        return -lerp_i64(now, a.sent_ns, a.seen_ns - a.sent_ns, b.sent_ns, b.seen_ns - b.sent_ns);
    }
    return lerp_i64(now, a.sent_ns, a.exch_ns - a.sent_ns, b.sent_ns, b.exch_ns - b.sent_ns);
}

inline int64_t back_delay(const LagPoint* s, int n, int& cursor, int64_t exch_now) noexcept {
    if (!s || n <= 0) return 0;
    if (cursor < 0) cursor = 0;
    while (cursor < n && s[cursor].exch_ns <= 0) ++cursor;
    if (cursor >= n) return 0;
    if (exch_now < s[cursor].exch_ns) {
        const int64_t lag = s[cursor].seen_ns - s[cursor].exch_ns;
        return lag < 0 ? 0 : lag;
    }
    for (;;) {
        int nxt = cursor + 1;
        while (nxt < n && s[nxt].exch_ns <= 0) ++nxt;
        if (nxt >= n) {
            const int64_t lag = s[cursor].seen_ns - s[cursor].exch_ns;
            return lag < 0 ? 0 : lag;
        }
        if (exch_now < s[nxt].exch_ns) {
            const int64_t lag = lerp_i64(exch_now, s[cursor].exch_ns, s[cursor].seen_ns - s[cursor].exch_ns,
                                         s[nxt].exch_ns, s[nxt].seen_ns - s[nxt].exch_ns);
            return lag < 0 ? 0 : lag;
        }
        cursor = nxt;
    }
}

struct TapeStamp {
    int64_t exch_ns{0};
    int64_t local_ns{0};
};

// If the smallest local-minus-exchange gap is negative, shift every local stamp
// so the new minimum equals base_ns. Gaps between rows stay the same.
inline int64_t lift_locals(TapeStamp* rows, int n, int64_t base_ns) noexcept {
    if (!rows || n <= 0) return 0;
    int64_t worst = rows[0].local_ns - rows[0].exch_ns;
    for (int i = 1; i < n; ++i) {
        const int64_t gap = rows[i].local_ns - rows[i].exch_ns;
        if (gap < worst) worst = gap;
    }
    if (worst >= 0) return 0;
    const int64_t shift = -worst + base_ns;
    for (int i = 0; i < n; ++i) rows[i].local_ns += shift;
    return shift;
}

struct DueStamp {
    uint64_t id{0};
    int64_t when{0};
};

// Later sends stay behind earlier ones. A timestamp earlier than the tail is clamped.
inline bool push_monotonic(DueStamp* q, int& n, int cap, uint64_t id, int64_t when) noexcept {
    if (!q || n >= cap) return false;
    if (n > 0 && when < q[n - 1].when) when = q[n - 1].when;
    q[n++] = DueStamp{id, when};
    return true;
}

}  // namespace pitlane
