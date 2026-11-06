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

// Days to subtract from the calendar date so a night print lands on the session date.
// Day session (08:00 through 18:00) returns 0.
int night_back_days(int hhmmss, int weekday) noexcept;
int session_date(int ymd, int hhmmss, int weekday, const ClosedDays* closed) noexcept;

}  // namespace pitlane
