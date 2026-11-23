// Factor laboratory: grid sweep over kind/window/horizon with IC, rank-IC,
// hit rate, turnover, ICIR and a turnover-adjusted score. Cold path only:
// fixed capacity, no heap, no exceptions.

#pragma once

#include "pitlane/desk/types.hpp"
#include "pitlane/study/factor.hpp"
#include "pitlane/study/factor_eval.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pitlane {

struct LabRow {
    uint8_t kind{0};
    uint16_t window{0};
    int horizon{0};
    int64_t n{0};
    int64_t ic_ppm{0};
    int64_t rank_ic_ppm{0};
    int64_t hit_ppm{0};
    int64_t top_ppm{0};
    int64_t bottom_ppm{0};
    int64_t turnover_ppm{0};
    int64_t icir_milli{0};  // mean(segment rank-IC) / sd * 1000
    int64_t score_ppm{0};   // |rank_ic| minus a turnover penalty
};

struct LabPick {
    LabRow row{};
    int8_t sign{1};
    int64_t threshold_ppm{1};
    int64_t trigger{0};
    int64_t ready{0};
};

class FactorLab {
public:
    static constexpr int kMaxRows = 96;
    static constexpr int kSegments = 8;
    static constexpr int kPickCap = 8192;

    // Fills up to `cap` rows (at most kMaxRows) and returns the count.
    int sweep(const Quote* tape, int n, int stride, int t_begin, int t_end, LabRow* out, int cap) noexcept {
        if (!tape || !out || n < 2 || cap <= 0) return 0;
        if (cap > kMaxRows) cap = kMaxRows;
        static const uint8_t kKinds[6] = {2, 4, 5, 6, 7, 8};
        static const uint16_t kWindows[5] = {4, 8, 16, 32, 64};
        static const int kHorizons[3] = {1, 4, 16};
        int nr = 0;
        for (int ki = 0; ki < 6 && nr < cap; ++ki) {
            for (int wi = 0; wi < 5 && nr < cap; ++wi) {
                for (int hi = 0; hi < 3 && nr < cap; ++hi) {
                    LabRow row{};
                    if (eval_combo(tape, n, kKinds[ki], kWindows[wi], kHorizons[hi], stride, t_begin,
                                   t_end, row)) {
                        out[nr++] = row;
                    }
                }
            }
        }
        sort(out, nr);
        return nr;
    }

    bool eval_combo(const Quote* tape, int n, uint8_t kind, uint16_t window, int horizon, int stride,
                    int t_begin, int t_end, LabRow& out) noexcept {
        out = LabRow{};
        if (!tape || n < 2 || horizon < 1) return false;
        if (t_end < 0 || t_end > n) t_end = n;
        if (t_begin < 0) t_begin = 0;
        if (t_begin >= t_end || stride < 1) return false;

        FactorEngine<1, 256> eng;
        FactorSpec spec{};
        spec.kind = kind;
        spec.window = window;
        spec.scale = 1000000;
        if (!eng.add(spec)) return false;

        FactorEval<8192> ev;
        FactorEval<512> seg[kSegments];
        int observed = 0;
        int changes = 0;
        int8_t last_pos = 0;
        const int span = t_end - t_begin;

        for (int t = 0; t < t_end; ++t) {
            const Quote& q = tape[t];
            const int64_t mid = lab_mid(q);
            if (mid <= 0) continue;
            if (!eng.on_sample(mid, q.exch_us)) continue;
            if (!eng.on_quote(q.bid_px, q.ask_px, q.bid_qty, q.ask_qty, q.exch_us)) continue;
            if (t < t_begin || (t % stride) != 0) continue;
            if (t + horizon >= n) break;
            const int64_t p1 = lab_mid(tape[t + horizon]);
            if (p1 <= 0) continue;
            FactorValue v{};
            if (!eng.value(0, v) || !v.ready) continue;
            const int64_t fwd = (p1 - mid) * 1000000 / mid;
            (void)ev.observe(v.value, fwd);
            int seg_i = span > 0 ? (t - t_begin) * kSegments / span : 0;
            if (seg_i < 0) seg_i = 0;
            if (seg_i >= kSegments) seg_i = kSegments - 1;
            (void)seg[seg_i].observe(v.value, fwd);
            const int8_t pos = v.value > 0 ? 1 : (v.value < 0 ? -1 : 0);
            if (observed > 0 && pos != last_pos) ++changes;
            last_pos = pos;
            ++observed;
        }

        FactorEval<8192>::Eval e{};
        if (!ev.result(e) || e.n < 1) return false;

        long double sum = 0;
        long double sq = 0;
        int seg_n = 0;
        for (int i = 0; i < kSegments; ++i) {
            FactorEval<512>::Eval se{};
            if (!seg[i].result(se) || se.n < 4) continue;
            const long double r = static_cast<long double>(se.rank_ic_ppm);
            sum += r;
            sq += r * r;
            ++seg_n;
        }
        int64_t icir = 0;
        if (seg_n >= 2) {
            const long double mean = sum / static_cast<long double>(seg_n);
            long double var = sq / static_cast<long double>(seg_n) - mean * mean;
            if (var < 0) var = 0;
            if (var > 0) {
                const long double sd = std::sqrt(static_cast<double>(var));
                icir = static_cast<int64_t>(mean * 1000.0L / sd);
            }
        }

        out.kind = kind;
        out.window = window;
        out.horizon = horizon;
        out.n = e.n;
        out.ic_ppm = e.ic_ppm;
        out.rank_ic_ppm = e.rank_ic_ppm;
        out.hit_ppm = e.hit_rate_ppm;
        out.top_ppm = e.top_quintile_ppm;
        out.bottom_ppm = e.bottom_quintile_ppm;
        out.turnover_ppm = observed > 0 ? static_cast<int64_t>(changes) * 1000000 / observed : 0;
        out.icir_milli = icir;
        out.score_ppm = score_of(out.rank_ic_ppm, out.turnover_ppm);
        return true;
    }

    // 75th percentile of |factor| on [0, t_end), at least one tick.
    bool pick(const Quote* tape, int n, const LabRow& row, int stride, int t_end, LabPick& out) noexcept {
        out = LabPick{};
        out.row = row;
        if (!tape || n < 2 || stride < 1) return false;
        if (t_end < 0 || t_end > n) t_end = n;
        if (row.kind < 1 || row.kind > 8 || row.window < 1 || row.window > 256) return false;

        FactorEngine<1, 256> eng;
        FactorSpec spec{};
        spec.kind = row.kind;
        spec.window = row.window;
        spec.scale = 1000000;
        if (!eng.add(spec)) return false;

        int64_t buf[kPickCap];
        int m = 0;
        int64_t ready = 0;
        for (int t = 0; t < t_end; ++t) {
            const Quote& q = tape[t];
            const int64_t mid = lab_mid(q);
            if (mid <= 0) continue;
            if (!eng.on_sample(mid, q.exch_us)) continue;
            if (!eng.on_quote(q.bid_px, q.ask_px, q.bid_qty, q.ask_qty, q.exch_us)) continue;
            if ((t % stride) != 0) continue;
            FactorValue v{};
            if (!eng.value(0, v) || !v.ready) continue;
            ++ready;
            const int64_t a = v.value < 0 ? -v.value : v.value;
            if (m < kPickCap) buf[m++] = a;
        }
        if (m < 8) return false;
        std::sort(buf, buf + m);
        int64_t threshold = buf[(m - 1) * 75 / 100];
        if (threshold < 1) threshold = 1;
        int64_t trigger = 0;
        for (int i = 0; i < m; ++i) {
            if (buf[i] >= threshold) ++trigger;
        }
        out.row = row;
        out.sign = row.rank_ic_ppm < 0 ? -1 : 1;
        out.threshold_ppm = threshold;
        out.trigger = trigger;
        out.ready = ready;
        return true;
    }

    // Score descending; ties break on |rank-IC|.
    static void sort(LabRow* rows, int n) noexcept {
        if (!rows || n < 2) return;
        for (int i = 1; i < n; ++i) {
            const LabRow key = rows[i];
            int j = i - 1;
            while (j >= 0 && (rows[j].score_ppm < key.score_ppm ||
                              (rows[j].score_ppm == key.score_ppm &&
                               iabs(rows[j].rank_ic_ppm) < iabs(key.rank_ic_ppm)))) {
                rows[j + 1] = rows[j];
                --j;
            }
            rows[j + 1] = key;
        }
    }

    static int64_t score_of(int64_t rank_ic_ppm, int64_t turnover_ppm) noexcept {
        return iabs(rank_ic_ppm) - turnover_ppm * 20 / 100;
    }

private:
    static int64_t iabs(int64_t v) noexcept { return v < 0 ? -v : v; }

    static int64_t lab_mid(const Quote& q) noexcept {
        if (q.bid_px > 0 && q.ask_px > 0) return (q.bid_px + q.ask_px) / 2;
        return q.last_px;
    }
};

}  // namespace pitlane
