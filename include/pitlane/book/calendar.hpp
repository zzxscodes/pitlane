#pragma once

#include <cstdint>

namespace pitlane {

bool ymd_valid(int ymd) noexcept;
int ymd_weekday(int ymd) noexcept;  // 1 = Monday .. 7 = Sunday, 0 if invalid
bool is_weekend(int ymd) noexcept;
int shift_ymd(int ymd, int days) noexcept;

class ClosedDays {
public:
    static constexpr int kCap = 512;
    bool add(int ymd) noexcept;
    bool has(int ymd) const noexcept;
    int size() const noexcept { return n_; }

private:
    int days_[kCap]{};
    int n_{0};
};

int next_session_day(int ymd, const ClosedDays* closed) noexcept;
int prev_session_day(int ymd, const ClosedDays* closed) noexcept;
int session_gap(int earlier, int later, const ClosedDays* closed) noexcept;

struct Span {
    int begin_min{0};
    int end_min{0};
};

int stock_spans(Span* out, int cap) noexcept;
int future_spans(bool night, int night_begin, int night_end, Span* out, int cap) noexcept;
bool minute_in_spans(int minute, const Span* spans, int n) noexcept;

struct SessionRow {
    char market[8]{};
    int open_min{0};
    int close_min{0};
    int lunch0{0};
    int lunch1{0};
};

bool minute_tradable(const SessionRow& row, int minute) noexcept;

// Index of the span that contains minute, or -1. An overnight span has end_min <= begin_min.
inline int span_index(int minute, const Span* spans, int n) noexcept {
    if (!spans || n <= 0) return -1;
    for (int i = 0; i < n; ++i) {
        const int b = spans[i].begin_min;
        const int e = spans[i].end_min;
        if (b == e) continue;
        if (b < e) {
            if (minute >= b && minute < e) return i;
        } else if (minute >= b || minute < e) {
            return i;
        }
    }
    return -1;
}

enum class SessionPart : uint8_t { Closed = 0, Open = 1, Lunch = 2 };

inline SessionPart session_part(const SessionRow& row, int minute) noexcept {
    if (row.lunch1 > row.lunch0 && minute >= row.lunch0 && minute < row.lunch1) return SessionPart::Lunch;
    if (minute < row.open_min || minute >= row.close_min) return SessionPart::Closed;
    return SessionPart::Open;
}

// Days to subtract from the calendar date so a night print lands on the session date.
// Day session (08:00 through 18:00) returns 0.
int night_back_days(int hhmmss, int weekday) noexcept;
int session_date(int ymd, int hhmmss, int weekday, const ClosedDays* closed) noexcept;

}  // namespace pitlane
