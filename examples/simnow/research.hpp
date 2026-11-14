#pragma once

// Offline tape research: clean the CSV, sweep factor kind/window/horizon,
// evaluate with FactorEval (IC, rank IC, hit rate, quintile spreads), then
// write a research product that backtest and live both consume.

#include "study_conf.hpp"

#include "pitlane/desk/types.hpp"
#include "pitlane/study/factor.hpp"
#include "pitlane/study/factor_eval.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace simnow_study {

struct TapeStats {
    int rows{0};
    int skipped{0};
    int64_t first_us{0};
    int64_t last_us{0};
};

inline int64_t iabs64(int64_t v) noexcept { return v == INT64_MIN ? INT64_MAX : (v < 0 ? -v : v); }

// Loads the seven-column tape. Keeps one instrument: `want_name` when given,
// otherwise the first one seen. Skips malformed rows, non-positive prices,
// crossed books and non-increasing time.
inline bool tape_load_csv(const char* path, std::vector<pitlane::Quote>& out, TapeStats& st,
                          uint32_t want_name = 0) {
    st = TapeStats{};
    out.clear();
    if (!path || !path[0]) return false;
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "tape: open %s failed\n", path);
        return false;
    }
    char line[512];
    uint32_t name = want_name;
    int64_t last_us = 0;
    constexpr int kMaxRows = 1 << 20;
    while (std::fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#' || *p == '\n' || *p == '\r') continue;
        long long t = 0, nm = 0, bp = 0, ap = 0, lp = 0, bq = 0, aq = 0;
        if (std::sscanf(p, "%lld,%lld,%lld,%lld,%lld,%lld,%lld", &t, &nm, &bp, &ap, &lp, &bq, &aq) != 7) {
            ++st.skipped;
            continue;
        }
        if (nm <= 0 || nm > 0xffffffffll || bp <= 0 || ap <= 0 || ap < bp || lp <= 0) {
            ++st.skipped;
            continue;
        }
        if (name == 0) name = static_cast<uint32_t>(nm);
        if (static_cast<uint32_t>(nm) != name) {
            ++st.skipped;
            continue;
        }
        if (t <= last_us) {
            ++st.skipped;
            continue;
        }
        if (static_cast<int>(out.size()) >= kMaxRows) {
            ++st.skipped;
            continue;
        }
        pitlane::Quote q{};
        q.exch_us = t;
        q.local_us = t;
        q.name = name;
        q.bid_px = bp;
        q.ask_px = ap;
        q.last_px = lp;
        q.bid_qty = bq;
        q.ask_qty = aq;
        out.push_back(q);
        last_us = t;
    }
    std::fclose(f);
    st.rows = static_cast<int>(out.size());
    if (!out.empty()) {
        st.first_us = out.front().exch_us;
        st.last_us = out.back().exch_us;
    }
    return !out.empty();
}

struct GridRow {
    uint8_t kind = 0;
    uint16_t window = 0;
    int horizon = 0;
    int64_t n = 0;
    int64_t ic = 0;
    int64_t rank_ic = 0;
    int64_t hit = 0;
    int64_t top = 0;
    int64_t bottom = 0;
    int64_t turnover_ppm = 0;  // sign changes per observation
    int64_t score_ppm = 0;     // |rank_ic| minus a turnover penalty
};

inline int64_t turnover_adjusted(int64_t rank_ic, int64_t turnover_ppm) noexcept {
    constexpr int64_t kWeightPct = 20;
    return iabs64(rank_ic) - turnover_ppm * kWeightPct / 100;
}

inline int64_t mid_px(const pitlane::Quote& q) noexcept {
    if (q.bid_px > 0 && q.ask_px > 0) return (q.bid_px + q.ask_px) / 2;
    return q.last_px;
}

// One pass over the tape for a single (kind, window, horizon). The engine sees
// the whole tape so indicators are warm, but only [t_begin, t_end) is observed.
inline bool eval_combo(const std::vector<pitlane::Quote>& tape, const std::vector<int64_t>& mid, uint8_t kind,
                       uint16_t window, int horizon, int stride, GridRow& row, int t_begin = 0, int t_end = -1) {
    pitlane::FactorEngine<1, 2048> eng;
    pitlane::FactorSpec spec{};
    spec.kind = kind;
    spec.window = window;
    spec.scale = 1000000;
    if (!eng.add(spec)) return false;
    pitlane::FactorEval<8192> ev;
    const int n = static_cast<int>(tape.size());
    if (t_end < 0 || t_end > n) t_end = n;
    int8_t last_pos = 0;
    int64_t changes = 0;
    int64_t observed = 0;
    for (int t = 0; t < n; ++t) {
        if (t >= t_end) break;
        const pitlane::Quote& q = tape[static_cast<std::size_t>(t)];
        if (!eng.on_sample(mid[static_cast<std::size_t>(t)], q.exch_us)) continue;
        if (!eng.on_quote(q.bid_px, q.ask_px, q.bid_qty, q.ask_qty, q.exch_us)) continue;
        if (t < t_begin || (t % stride) != 0) continue;
        if (t + horizon >= n) break;
        pitlane::FactorValue v{};
        if (!eng.value(0, v) || !v.ready) continue;
        const int64_t p0 = mid[static_cast<std::size_t>(t)];
        const int64_t p1 = mid[static_cast<std::size_t>(t + horizon)];
        if (p0 <= 0) continue;
        const int64_t fwd = (p1 - p0) * 1000000 / p0;
        (void)ev.observe(v.value, fwd);
        const int8_t pos = v.value > 0 ? 1 : (v.value < 0 ? -1 : 0);
        if (observed > 0 && pos != last_pos) ++changes;
        last_pos = pos;
        ++observed;
    }
    pitlane::FactorEval<8192>::Eval e{};
    if (!ev.result(e)) return false;
    row.kind = kind;
    row.window = window;
    row.horizon = horizon;
    row.n = e.n;
    row.ic = e.ic_ppm;
    row.rank_ic = e.rank_ic_ppm;
    row.hit = e.hit_rate_ppm;
    row.top = e.top_quintile_ppm;
    row.bottom = e.bottom_quintile_ppm;
    row.turnover_ppm = observed > 0 ? changes * 1000000 / observed : 0;
    row.score_ppm = turnover_adjusted(row.rank_ic, row.turnover_ppm);
    return true;
}

// Fraction of samples where the traded position changes sign, on [t0, t1).
inline int64_t eval_turnover(const std::vector<pitlane::Quote>& tape, const std::vector<int64_t>& mid, uint8_t kind,
                             uint16_t window, int8_t sign, int64_t threshold_ppm, int stride, int t0, int t1) {
    pitlane::FactorEngine<1, 2048> eng;
    pitlane::FactorSpec spec{};
    spec.kind = kind;
    spec.window = window;
    spec.scale = 1000000;
    if (!eng.add(spec)) return 0;
    const int n = static_cast<int>(tape.size());
    if (t1 < 0 || t1 > n) t1 = n;
    int8_t last_pos = 0;
    int64_t changes = 0;
    int64_t observed = 0;
    for (int t = 0; t < t1; ++t) {
        const pitlane::Quote& q = tape[static_cast<std::size_t>(t)];
        if (!eng.on_sample(mid[static_cast<std::size_t>(t)], q.exch_us)) continue;
        if (!eng.on_quote(q.bid_px, q.ask_px, q.bid_qty, q.ask_qty, q.exch_us)) continue;
        if (t < t0 || (t % stride) != 0) continue;
        pitlane::FactorValue v{};
        if (!eng.value(0, v) || !v.ready) continue;
        const int64_t directional = static_cast<int64_t>(sign) * v.value;
        const int8_t pos = directional >= threshold_ppm ? 1 : (directional <= -threshold_ppm ? -1 : 0);
        if (observed > 0 && pos != last_pos) ++changes;
        last_pos = pos;
        ++observed;
    }
    return observed > 0 ? changes * 1000000 / observed : 0;
}

// 75th percentile of |factor| for the chosen combo: roughly 25% of ready
// samples trigger, and the value is at least one tick to keep signals alive.
inline bool pick_threshold(const std::vector<pitlane::Quote>& tape, const std::vector<int64_t>& mid, uint8_t kind,
                           uint16_t window, int stride, int64_t& threshold_ppm, int64_t& trigger, int64_t& ready_n,
                           int t_end = -1) {
    pitlane::FactorEngine<1, 2048> eng;
    pitlane::FactorSpec spec{};
    spec.kind = kind;
    spec.window = window;
    spec.scale = 1000000;
    if (!eng.add(spec)) return false;
    std::vector<int64_t> absv;
    absv.reserve(tape.size());
    const int n = static_cast<int>(tape.size());
    if (t_end < 0 || t_end > n) t_end = n;
    for (int t = 0; t < t_end; ++t) {
        const pitlane::Quote& q = tape[static_cast<std::size_t>(t)];
        if (!eng.on_sample(mid[static_cast<std::size_t>(t)], q.exch_us)) continue;
        if (!eng.on_quote(q.bid_px, q.ask_px, q.bid_qty, q.ask_qty, q.exch_us)) continue;
        if ((t % stride) != 0) continue;
        pitlane::FactorValue v{};
        if (!eng.value(0, v) || !v.ready) continue;
        absv.push_back(iabs64(v.value));
    }
    ready_n = static_cast<int64_t>(absv.size());
    if (absv.empty()) return false;
    std::sort(absv.begin(), absv.end());
    threshold_ppm = absv[(absv.size() - 1) * 75 / 100];
    if (threshold_ppm < 1) threshold_ppm = 1;
    trigger = 0;
    for (int64_t v : absv) {
        if (v >= threshold_ppm) ++trigger;
    }
    return true;
}

inline bool research_run(const std::vector<pitlane::Quote>& tape, const char* out_path, pitlane::StudyCfg& best) {
    const int n = static_cast<int>(tape.size());
    if (n < 64) {
        std::fprintf(stderr, "research: tape too short (%d rows, need >= 64)\n", n);
        return false;
    }
    std::vector<int64_t> mid(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) mid[static_cast<std::size_t>(i)] = mid_px(tape[static_cast<std::size_t>(i)]);
    const int stride = n / 4096 > 1 ? n / 4096 : 1;
    const int split = n * 70 / 100;  // train on the first 70%, validate on the last 30%

    const uint8_t kinds[] = {2, 4, 5, 6, 7, 8}; // Momentum ZScore Ema Rsi Ofi Imbalance
    const uint16_t windows[] = {4, 8, 16, 32, 64};
    const int horizons[] = {1, 4, 16};

    std::vector<GridRow> rows;
    rows.reserve(90);
    for (uint8_t k : kinds) {
        for (uint16_t w : windows) {
            for (int h : horizons) {
                GridRow r{};
                if (eval_combo(tape, mid, k, w, h, stride, r, 0, split) && r.n >= 16) rows.push_back(r);
            }
        }
    }
    if (rows.empty()) {
        std::fprintf(stderr, "research: no combo reached 16 in-sample observations\n");
        return false;
    }
    std::sort(rows.begin(), rows.end(), [](const GridRow& a, const GridRow& b) {
        if (a.score_ppm != b.score_ppm) return a.score_ppm > b.score_ppm;
        return iabs64(a.rank_ic) > iabs64(b.rank_ic);
    });

    std::printf("\n=== pitlane research grid in-sample 70%% (top %d of %d) ===\n",
                rows.size() < 10 ? static_cast<int>(rows.size()) : 10, static_cast<int>(rows.size()));
    std::printf("kind  win  h     n     ic_ppm  rank_ic  hit_ppm  turn_ppm score\n");
    const int shown = static_cast<int>(rows.size() < 10 ? rows.size() : 10);
    for (int i = 0; i < shown; ++i) {
        const GridRow& r = rows[static_cast<std::size_t>(i)];
        std::printf("%-5u %-4u %-5d %-5lld %-7lld %-8lld %-8lld %-8lld %lld\n", static_cast<unsigned>(r.kind),
                    static_cast<unsigned>(r.window), r.horizon, static_cast<long long>(r.n), static_cast<long long>(r.ic),
                    static_cast<long long>(r.rank_ic), static_cast<long long>(r.hit),
                    static_cast<long long>(r.turnover_ppm), static_cast<long long>(r.score_ppm));
    }

    const GridRow& top = rows.front();
    int64_t threshold = 1;
    int64_t trigger = 0;
    int64_t ready_n = 0;
    if (!pick_threshold(tape, mid, top.kind, top.window, stride, threshold, trigger, ready_n, split)) {
        std::fprintf(stderr, "research: threshold pass failed\n");
        return false;
    }

    // The held-out last 30% is never used for selection or thresholding.
    GridRow oos{};
    const bool have_oos = eval_combo(tape, mid, top.kind, top.window, top.horizon, stride, oos, split, n) && oos.n >= 16;
    const int8_t sign = top.rank_ic < 0 ? -1 : 1;
    const int64_t oos_turnover = eval_turnover(tape, mid, top.kind, top.window, sign, threshold, stride, split, n);

    best = pitlane::StudyCfg{};
    best.kind = top.kind;
    best.window = top.window;
    best.sign = sign;
    best.signal_ppm = threshold;
    best.horizon_bars = top.horizon;
    best.samples = top.n;
    best.ic_ppm = top.ic;
    best.rank_ic_ppm = top.rank_ic;
    best.hit_rate_ppm = top.hit;
    best.oos_rank_ic_ppm = have_oos ? oos.rank_ic : 0;
    best.oos_samples = have_oos ? oos.n : 0;
    best.turnover_ppm = oos_turnover;
    best.score_ppm = have_oos ? turnover_adjusted(oos.rank_ic, oos_turnover) : 0;

    if (!pitlane::study_conf_save(out_path, best)) return false;
    const int64_t pct = ready_n > 0 ? trigger * 1000 / ready_n : 0;
    std::printf("best kind=%u window=%u sign=%d horizon=%d signal_ppm=%lld samples=%lld ic=%lld rank_ic=%lld\n",
                static_cast<unsigned>(best.kind), static_cast<unsigned>(best.window), static_cast<int>(best.sign),
                best.horizon_bars, static_cast<long long>(best.signal_ppm), static_cast<long long>(best.samples),
                static_cast<long long>(best.ic_ppm), static_cast<long long>(best.rank_ic_ppm));
    std::printf("out-of-sample n=%lld rank_ic=%lld turnover_ppm=%lld score=%lld (70/30 split)\n",
                static_cast<long long>(best.oos_samples), static_cast<long long>(best.oos_rank_ic_ppm),
                static_cast<long long>(best.turnover_ppm), static_cast<long long>(best.score_ppm));
    std::printf("threshold trigger=%lld/%lld (%.1f%%); product written to %s\n", static_cast<long long>(trigger),
                static_cast<long long>(ready_n), static_cast<double>(pct) / 10.0, out_path);
    return true;
}

} // namespace simnow_study
