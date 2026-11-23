#pragma once

// Period performance from a NAV track. The track is a fixed ring of settled
// snapshots; a report folds the points at or after a period start into a
// single row: return, annualised return, drawdown, hit rate, benchmark excess,
// tracking error, and information ratio.
//
// Returns are integer ppm. Volatility is the mean absolute period return, which
// keeps the whole report in integer arithmetic and is robust to a single large
// print. The console feeds the track from clearing snapshots on its own thread;
// the trading thread never calls this header.

#include <cstdint>
#include <cstdio>
#include <ctime>

namespace pitlane {

inline constexpr int64_t kRptPpm = 1000000;

enum class RptPeriod : uint8_t { Daily = 0, Weekly = 1, Monthly = 2, Yearly = 3 };

inline const char* rpt_period_name(uint8_t p) noexcept {
    switch (static_cast<RptPeriod>(p)) {
        case RptPeriod::Weekly: return "weekly";
        case RptPeriod::Monthly: return "monthly";
        case RptPeriod::Yearly: return "yearly";
        default: return "daily";
    }
}

struct RptPoint {
    int64_t ts_us{0};
    int64_t nav_ppm{kRptPpm};
    int64_t benchmark_ppm{kRptPpm};
    int64_t equity{0};
    int64_t fees{0};
};

struct RptStats {
    int points{0};
    int intervals{0};
    int64_t first_us{0};
    int64_t last_us{0};
    int64_t return_ppm{0};
    int64_t annual_ppm{0};
    int64_t mean_ppm{0};
    int64_t volatility_ppm{0};
    int64_t sharpe_ppm{0};
    int64_t max_drawdown_ppm{0};
    int64_t realtime_drawdown_ppm{0};
    int64_t win_ppm{0};
    int64_t best_ppm{0};
    int64_t worst_ppm{0};
    int64_t benchmark_ppm{0};
    int64_t excess_ppm{0};
    int64_t tracking_ppm{0};
    int64_t info_ppm{0};
    int64_t fees{0};
};

// Fixed ring of report points. A later point with the same timestamp replaces
// the previous one, so a beat that samples twice does not double count.
template <int Cap = 512>
class NavTrack {
public:
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "track capacity");

    void clear() noexcept {
        head_ = 0;
        n_ = 0;
    }

    int size() const noexcept { return n_; }

    bool push(const RptPoint& p) noexcept {
        if (p.ts_us <= 0 || p.nav_ppm <= 0) return false;
        if (n_ > 0) {
            const RptPoint& last = at(n_ - 1);
            if (p.ts_us < last.ts_us) return false;
            if (p.ts_us == last.ts_us) {
                buf_[(head_ + Cap - 1) % Cap] = p;
                return true;
            }
        }
        buf_[head_] = p;
        head_ = (head_ + 1) % Cap;
        if (n_ < Cap) ++n_;
        return true;
    }

    // Index 0 is the oldest retained point.
    RptPoint at(int i) const noexcept {
        const int start = (head_ + Cap - n_) % Cap;
        return buf_[(start + i) % Cap];
    }

    int since(int64_t since_us, RptPoint* out, int cap) const noexcept {
        if (!out || cap <= 0) return 0;
        int k = 0;
        for (int i = 0; i < n_ && k < cap; ++i) {
            const RptPoint p = at(i);
            if (p.ts_us >= since_us) out[k++] = p;
        }
        return k;
    }

private:
    RptPoint buf_[Cap]{};
    int head_{0};
    int n_{0};
};

// Start of the period that contains `now_us`, in UTC. Weekly starts on Monday.
inline int64_t rpt_since(uint8_t period, int64_t now_us) noexcept {
    if (now_us <= 0) return 0;
    const time_t sec = static_cast<time_t>(now_us / 1000000);
    std::tm tm{};
    if (!gmtime_r(&sec, &tm)) return 0;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    switch (static_cast<RptPeriod>(period)) {
        case RptPeriod::Weekly: {
            const int back = (tm.tm_wday + 6) % 7;  // Monday = 0
            tm.tm_mday -= back;
            break;
        }
        case RptPeriod::Monthly:
            tm.tm_mday = 1;
            break;
        case RptPeriod::Yearly:
            tm.tm_mon = 0;
            tm.tm_mday = 1;
            break;
        default:
            break;
    }
    const time_t start = timegm(&tm);
    return start > 0 ? static_cast<int64_t>(start) * 1000000 : 0;
}

namespace report_detail {

inline int64_t ratio_ppm(int64_t num, int64_t den) noexcept {
    if (den <= 0) return 0;
    if (num > INT64_MAX / kRptPpm || num < INT64_MIN / kRptPpm) return num > 0 ? INT64_MAX : INT64_MIN;
    return num * kRptPpm / den;
}

inline int64_t abs64(int64_t v) noexcept { return v < 0 ? -v : v; }

}  // namespace report_detail

// Folds the points at or after `since_us`. A series with fewer than two points
// reports the last NAV and zero flows; the caller decides whether that is a
// usable row.
template <int Cap>
inline bool report_fold(const NavTrack<Cap>& track, int64_t since_us, RptStats& out) noexcept {
    out = RptStats{};
    RptPoint p[Cap]{};
    const int n = track.since(since_us, p, Cap);
    if (n <= 0) return false;
    out.points = n;
    out.first_us = p[0].ts_us;
    out.last_us = p[n - 1].ts_us;
    out.fees = p[n - 1].fees;
    if (n < 2) {
        out.realtime_drawdown_ppm = 0;
        return true;
    }

    const int64_t base = p[0].nav_ppm;
    const int64_t last = p[n - 1].nav_ppm;
    if (base <= 0) return false;
    out.return_ppm = report_detail::ratio_ppm(last - base, base);
    out.benchmark_ppm = report_detail::ratio_ppm(p[n - 1].benchmark_ppm - p[0].benchmark_ppm, p[0].benchmark_ppm);
    out.excess_ppm = out.return_ppm - out.benchmark_ppm;

    int64_t peak = base;
    int64_t sum = 0;
    int64_t abs_sum = 0;
    int64_t active_sum = 0;
    int64_t active_abs = 0;
    int wins = 0;
    out.intervals = n - 1;
    for (int i = 1; i < n; ++i) {
        const int64_t prev = p[i - 1].nav_ppm;
        const int64_t cur = p[i].nav_ppm;
        const int64_t r = report_detail::ratio_ppm(cur - prev, prev);
        sum += r;
        abs_sum += report_detail::abs64(r);
        if (r > 0) ++wins;
        if (r > out.best_ppm) out.best_ppm = r;
        if (r < out.worst_ppm) out.worst_ppm = r;
        const int64_t rb =
            report_detail::ratio_ppm(p[i].benchmark_ppm - p[i - 1].benchmark_ppm, p[i - 1].benchmark_ppm);
        active_sum += r - rb;
        active_abs += report_detail::abs64(r - rb);
        if (cur > peak) peak = cur;
        if (peak > 0) {
            const int64_t dd = report_detail::ratio_ppm(peak - cur, peak);
            if (dd > out.max_drawdown_ppm) out.max_drawdown_ppm = dd;
        }
    }
    const int k = out.intervals;
    out.mean_ppm = sum / k;
    out.volatility_ppm = abs_sum / k;
    out.sharpe_ppm = out.volatility_ppm > 0 ? out.mean_ppm * kRptPpm / out.volatility_ppm : 0;
    out.win_ppm = static_cast<int64_t>(wins) * kRptPpm / k;
    out.tracking_ppm = active_abs / k;
    out.info_ppm = out.tracking_ppm > 0 ? (active_sum / k) * kRptPpm / out.tracking_ppm : 0;
    if (peak > 0 && last < peak) out.realtime_drawdown_ppm = (peak - last) * kRptPpm / peak;

    const int64_t days = (out.last_us - out.first_us) / 86400000000ll;
    if (days > 0) {
        if (out.return_ppm > INT64_MAX / 365) {
            out.annual_ppm = INT64_MAX;
        } else if (out.return_ppm < INT64_MIN / 365) {
            out.annual_ppm = INT64_MIN;
        } else {
            out.annual_ppm = out.return_ppm * 365 / days;
        }
    } else {
        out.annual_ppm = out.return_ppm;
    }
    return true;
}

inline bool report_csv(const RptStats& s, const char* path) noexcept {
    if (!path || !path[0]) return false;
    std::FILE* f = std::fopen(path, "w");
    if (!f) return false;
    std::fprintf(f,
                 "points,intervals,first_us,last_us,return_ppm,annual_ppm,mean_ppm,volatility_ppm,"
                 "sharpe_ppm,max_drawdown_ppm,realtime_drawdown_ppm,win_ppm,best_ppm,worst_ppm,"
                 "benchmark_ppm,excess_ppm,tracking_ppm,info_ppm,fees\n");
    std::fprintf(f, "%d,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
                 s.points, s.intervals, static_cast<long long>(s.first_us), static_cast<long long>(s.last_us),
                 static_cast<long long>(s.return_ppm), static_cast<long long>(s.annual_ppm),
                 static_cast<long long>(s.mean_ppm), static_cast<long long>(s.volatility_ppm),
                 static_cast<long long>(s.sharpe_ppm), static_cast<long long>(s.max_drawdown_ppm),
                 static_cast<long long>(s.realtime_drawdown_ppm), static_cast<long long>(s.win_ppm),
                 static_cast<long long>(s.best_ppm), static_cast<long long>(s.worst_ppm),
                 static_cast<long long>(s.benchmark_ppm), static_cast<long long>(s.excess_ppm),
                 static_cast<long long>(s.tracking_ppm), static_cast<long long>(s.info_ppm),
                 static_cast<long long>(s.fees));
    std::fclose(f);
    return true;
}

}  // namespace pitlane
