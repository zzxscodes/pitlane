#include "pitlane/book/calendar.hpp"

namespace pitlane {
namespace {

bool leap(int y) noexcept { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

int month_days(int y, int m) noexcept {
    static constexpr int kDay[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && leap(y)) return 29;
    return kDay[m];
}

int to_days(int y, int m, int d) noexcept {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

void from_days(int z, int& y, int& m, int& d) noexcept {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    y += (m <= 2);
}

bool blocked(int ymd, const ClosedDays* closed) noexcept {
    return is_weekend(ymd) || (closed && closed->has(ymd));
}

}  // namespace

bool ymd_valid(int ymd) noexcept {
    const int y = ymd / 10000;
    const int m = (ymd / 100) % 100;
    const int d = ymd % 100;
    if (y < 1970 || y > 2100 || m < 1 || m > 12 || d < 1) return false;
    return d <= month_days(y, m);
}

int ymd_weekday(int ymd) noexcept {
    if (!ymd_valid(ymd)) return 0;
    const int y0 = ymd / 10000;
    const int m = (ymd / 100) % 100;
    const int d = ymd % 100;
    int y = y0;
    static constexpr int kT[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) --y;
    const int w = (y + y / 4 - y / 100 + y / 400 + kT[m - 1] + d) % 7;
    return w == 0 ? 7 : w;
}

bool is_weekend(int ymd) noexcept {
    const int w = ymd_weekday(ymd);
    return w == 6 || w == 7;
}

int shift_ymd(int ymd, int days) noexcept {
    if (!ymd_valid(ymd)) return 0;
    int y = 0, m = 0, d = 0;
    from_days(to_days(ymd / 10000, (ymd / 100) % 100, ymd % 100) + days, y, m, d);
    return y * 10000 + m * 100 + d;
}

bool ClosedDays::add(int ymd) noexcept {
    if (!ymd_valid(ymd) || has(ymd) || n_ >= kCap) return false;
    days_[n_++] = ymd;
    return true;
}

bool ClosedDays::has(int ymd) const noexcept {
    for (int i = 0; i < n_; ++i) {
        if (days_[i] == ymd) return true;
    }
    return false;
}

int next_session_day(int ymd, const ClosedDays* closed) noexcept {
    if (!ymd_valid(ymd)) return 0;
    for (int i = 1; i <= 366; ++i) {
        const int d = shift_ymd(ymd, i);
        if (!blocked(d, closed)) return d;
    }
    return 0;
}

int prev_session_day(int ymd, const ClosedDays* closed) noexcept {
    if (!ymd_valid(ymd)) return 0;
    for (int i = 1; i <= 366; ++i) {
        const int d = shift_ymd(ymd, -i);
        if (!blocked(d, closed)) return d;
    }
    return 0;
}

int session_gap(int earlier, int later, const ClosedDays* closed) noexcept {
    if (!ymd_valid(earlier) || !ymd_valid(later) || later < earlier) return 0;
    int n = 0;
    int d = earlier;
    while (d < later) {
        d = next_session_day(d, closed);
        if (d == 0 || d > later) break;
        ++n;
    }
    return n;
}

int stock_spans(Span* out, int cap) noexcept {
    if (!out || cap < 2) return 0;
    out[0] = Span{9 * 60 + 30, 11 * 60 + 30};
    out[1] = Span{13 * 60, 15 * 60};
    return 2;
}

int future_spans(bool night, int night_begin, int night_end, Span* out, int cap) noexcept {
    if (!out || cap < 3) return 0;
    int n = 0;
    if (night && n < cap) out[n++] = Span{night_begin, night_end};
    if (n < cap) out[n++] = Span{9 * 60, 10 * 60 + 15};
    if (n < cap) out[n++] = Span{10 * 60 + 30, 11 * 60 + 30};
    if (n < cap) out[n++] = Span{13 * 60 + 30, 15 * 60};
    return n;
}

bool minute_in_spans(int minute, const Span* spans, int n) noexcept {
    if (!spans || n <= 0) return false;
    for (int i = 0; i < n; ++i) {
        const int b = spans[i].begin_min;
        const int e = spans[i].end_min;
        if (b == e) continue;
        if (b < e) {
            if (minute >= b && minute < e) return true;
        } else if (minute >= b || minute < e) {
            return true;
        }
    }
    return false;
}

bool minute_tradable(const SessionRow& row, int minute) noexcept {
    if (minute < row.open_min || minute >= row.close_min) return false;
    if (row.lunch1 > row.lunch0 && minute >= row.lunch0 && minute < row.lunch1) return false;
    return true;
}

int night_back_days(int hhmmss, int weekday) noexcept {
    if (hhmmss >= 80000 && hhmmss <= 180000) return 0;
    if (hhmmss > 180000) return weekday == 1 ? 3 : 1;
    return weekday == 1 ? 2 : 1;
}

int session_date(int ymd, int hhmmss, int weekday, const ClosedDays* closed) noexcept {
    const int back = night_back_days(hhmmss, weekday);
    int d = back == 0 ? ymd : shift_ymd(ymd, -back);
    if (!ymd_valid(d)) return 0;
    while (blocked(d, closed)) {
        const int prev = prev_session_day(d, closed);
        if (prev == 0) return d;
        d = prev;
    }
    return d;
}

}  // namespace pitlane
