#pragma once

#include <cmath>
#include <cstdint>

namespace pitlane {

// Integer equity-curve statistics. Cold path; no heap, long double only here.
// Sharpe is per sample and not annualised: it compares runs sampled at the
// same frequency. max_drawdown_ppm divides by max(1, |peak|, |min|), so a
// curve that starts near zero still yields a stable ratio.

template <int Cap = 4096>
class PerfCurve {
public:
    static_assert(Cap >= 2, "cap");

    struct Stats {
        int64_t n;
        int64_t start;
        int64_t end;
        int64_t peak;
        int64_t min;
        int64_t max_drawdown;      // absolute PnL units, >= 0
        int64_t max_drawdown_ppm;  // maxdd * 1e6 / max(1, |peak|, |min|)
        int64_t sharpe_ppm;        // mean(d)/sd(d) * 1e6, per sample; 0 when sd == 0
    };

    void clear() noexcept { n_ = 0; }
    int size() const noexcept { return n_; }

    bool mark(int64_t equity) noexcept {
        if (n_ >= Cap) return false;
        eq_[n_++] = equity;
        return true;
    }

    bool result(Stats& out) const noexcept {
        const int n = n_;
        if (n < 2) return false;

        int64_t peak = eq_[0];
        int64_t min_v = eq_[0];
        int64_t run_peak = eq_[0];
        int64_t run_min = eq_[0];
        int64_t max_dd = 0;
        for (int i = 0; i < n; ++i) {
            const int64_t v = eq_[i];
            if (v > peak) peak = v;
            if (v < min_v) min_v = v;
            if (v > run_peak) {
                run_peak = v;
                run_min = v;
            }
            if (v < run_min) run_min = v;
            const int64_t dd = run_peak - run_min;
            if (dd > max_dd) max_dd = dd;
        }

        long double sum = 0.0L;
        long double sumsq = 0.0L;
        for (int i = 1; i < n; ++i) {
            const long double d = static_cast<long double>(eq_[i] - eq_[i - 1]);
            sum += d;
            sumsq += d * d;
        }
        const long double dn = static_cast<long double>(n - 1);
        const long double mean = sum / dn;
        const long double var = sumsq / dn - mean * mean;
        int64_t sharpe = 0;
        if (var > 0.0L) {
            const long double sd = std::sqrt(var);
            if (sd > 0.0L) sharpe = static_cast<int64_t>(mean / sd * 1000000.0L);
        }

        const int64_t scale = peak > -min_v ? peak : -min_v;
        const int64_t denom = scale > 0 ? scale : 1;
        out.max_drawdown_ppm = max_dd * 1000000 / denom;
        out.n = n;
        out.start = eq_[0];
        out.end = eq_[n - 1];
        out.peak = peak;
        out.min = min_v;
        out.max_drawdown = max_dd;
        out.max_drawdown_ppm = max_dd * 1000000 / (denom > 0 ? denom : 1);
        out.sharpe_ppm = sharpe;
        return true;
    }

private:
    int64_t eq_[Cap]{};
    int n_{0};
};

} // namespace pitlane
