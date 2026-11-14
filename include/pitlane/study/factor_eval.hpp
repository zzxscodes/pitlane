#pragma once

#include "pitlane/util/hint.hpp"

#include <cmath>
#include <cstdint>

namespace pitlane {

// Forward-return evaluator for one integer factor series (study §22).
// Cold path: long double is used only for IC/rank-IC; no heap, no exceptions.
// Scratch buffers are members so `result` stays const and off the large stack.

template <int Cap = 1024>
class FactorEval {
public:
    static_assert(Cap >= 8 && Cap <= 8192, "cap");

    struct Eval {
        int64_t n;
        int64_t ic_ppm;
        int64_t rank_ic_ppm;
        int64_t hit_rate_ppm;
        int64_t mean_factor;
        int64_t mean_return;
        // Despite the _ppm name these two hold mean returns in the original
        // return units (integer division), as documented by the API.
        int64_t top_quintile_ppm;
        int64_t bottom_quintile_ppm;
    };

    void clear() noexcept { n_ = 0; }
    int size() const noexcept { return n_; }

    bool observe(int64_t factor, int64_t forward_return) noexcept {
        if (unlikely(n_ >= Cap)) return false;
        factor_[n_] = factor;
        ret_[n_] = forward_return;
        ++n_;
        return true;
    }

    // Requires n >= 3. Fails without touching `out` otherwise.
    bool result(Eval& out) const noexcept {
        const int n = n_;
        if (n < 3) return false;

        long double sumf = 0.0L;
        long double sumr = 0.0L;
        for (int i = 0; i < n; ++i) {
            sumf += static_cast<long double>(factor_[i]);
            sumr += static_cast<long double>(ret_[i]);
        }
        const long double dn = static_cast<long double>(n);
        const long double mf = sumf / dn;
        const long double mr = sumr / dn;

        long double cov = 0.0L;
        long double vf = 0.0L;
        long double vr = 0.0L;
        int64_t hits = 0;
        for (int i = 0; i < n; ++i) {
            const long double df = static_cast<long double>(factor_[i]) - mf;
            const long double dr = static_cast<long double>(ret_[i]) - mr;
            cov += df * dr;
            vf += df * df;
            vr += dr * dr;
            const int sf = (factor_[i] > 0) - (factor_[i] < 0);
            const int sr = (ret_[i] > 0) - (ret_[i] < 0);
            if (sf != 0 && sr != 0 && sf == sr) ++hits;
        }

        int64_t ic_ppm = 0;
        if (vf > 0.0L && vr > 0.0L) {
            const long double ic = cov / std::sqrt(vf * vr);
            ic_ppm = static_cast<int64_t>(ic * 1000000.0L);
        }
        const int64_t hit_rate_ppm = hits * 1000000 / n;

        // Spearman: rank both series, then Pearson on the ranks.
        for (int i = 0; i < n; ++i) {
            sf_[i] = factor_[i];
            sr_[i] = ret_[i];
            idx_[i] = i;
        }
        for (int i = 1; i < n; ++i) {
            const int key = idx_[i];
            int j = i - 1;
            while (j >= 0 && sf_[idx_[j]] > sf_[key]) {
                idx_[j + 1] = idx_[j];
                --j;
            }
            idx_[j + 1] = key;
        }
        for (int pos = 0; pos < n; ++pos) ranks_[idx_[pos]] = pos + 1;

        for (int i = 0; i < n; ++i) idx_[i] = i;
        for (int i = 1; i < n; ++i) {
            const int key = idx_[i];
            int j = i - 1;
            while (j >= 0 && sr_[idx_[j]] > sr_[key]) {
                idx_[j + 1] = idx_[j];
                --j;
            }
            idx_[j + 1] = key;
        }

        const long double mean_rank = static_cast<long double>(n + 1) / 2.0L;
        long double sumxy = 0.0L;
        long double sumxx = 0.0L;
        long double sumyy = 0.0L;
        for (int pos = 0; pos < n; ++pos) {
            const long double rx = static_cast<long double>(ranks_[idx_[pos]]);
            const long double ry = static_cast<long double>(pos + 1);
            sumxy += rx * ry;
            sumxx += rx * rx;
            sumyy += ry * ry;
        }
        const long double nl = static_cast<long double>(n);
        const long double cov_r = sumxy - nl * mean_rank * mean_rank;
        const long double vfr = sumxx - nl * mean_rank * mean_rank;
        const long double vrr = sumyy - nl * mean_rank * mean_rank;
        int64_t rank_ic_ppm = 0;
        if (vfr > 0.0L && vrr > 0.0L) {
            const long double ric = cov_r / std::sqrt(vfr * vrr);
            rank_ic_ppm = static_cast<int64_t>(ric * 1000000.0L);
        }

        // Quintiles: sort (factor, return) ascending by factor, then split by index.
        for (int i = 1; i < n; ++i) {
            const int64_t kf = sf_[i];
            const int64_t kr = sr_[i];
            int j = i - 1;
            while (j >= 0 && sf_[j] > kf) {
                sf_[j + 1] = sf_[j];
                sr_[j + 1] = sr_[j];
                --j;
            }
            sf_[j + 1] = kf;
            sr_[j + 1] = kr;
        }
        int g = n / 5;
        if (g < 1) g = 1;
        int64_t bottom = 0;
        int64_t top = 0;
        for (int i = 0; i < g; ++i) bottom = add_sat(bottom, sr_[i]);
        for (int i = n - g; i < n; ++i) top = add_sat(top, sr_[i]);
        bottom /= g;
        top /= g;

        out.n = n;
        out.ic_ppm = ic_ppm;
        out.rank_ic_ppm = rank_ic_ppm;
        out.hit_rate_ppm = hit_rate_ppm;
        out.mean_factor = static_cast<int64_t>(mf);
        out.mean_return = static_cast<int64_t>(mr);
        out.top_quintile_ppm = top;
        out.bottom_quintile_ppm = bottom;
        return true;
    }

private:
    static int64_t add_sat(int64_t a, int64_t b) noexcept {
        if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
        if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
        return a + b;
    }

    int64_t factor_[Cap]{};
    int64_t ret_[Cap]{};
    mutable int64_t sf_[Cap]{};
    mutable int64_t sr_[Cap]{};
    mutable int idx_[Cap]{};
    mutable int ranks_[Cap]{};
    int n_{0};
};

}  // namespace pitlane
