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

// Loads the seven-column tape. Keeps only the first instrument seen and skips
// malformed rows, non-positive prices, crossed books and non-increasing time.
inline bool tape_load_csv(const char* path, std::vector<pitlane::Quote>& out, TapeStats& st) {
    st = TapeStats{};
    out.clear();
    if (!path || !path[0]) return false;
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "tape: open %s failed\n", path);
        return false;
    }
    char line[512];
    uint32_t name = 0;
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
};

inline int64_t mid_px(const pitlane::Quote& q) noexcept {
    if (q.bid_px > 0 && q.ask_px > 0) return (q.bid_px + q.ask_px) / 2;
    return q.last_px;
}

// One pass over the tape for a single (kind, window, horizon).
inline bool eval_combo(const std::vector<pitlane::Quote>& tape, const std::vector<int64_t>& mid, uint8_t kind,
                       uint16_t window, int horizon, int stride, GridRow& row) {
    pitlane::FactorEngine<1, 2048> eng;
    pitlane::FactorSpec spec{};
    spec.kind = kind;
    spec.window = window;
    spec.scale = 1000000;
    if (!eng.add(spec)) return false;
    pitlane::FactorEval<8192> ev;
    const int n = static_cast<int>(tape.size());
    for (int t = 0; t < n; ++t) {
        const pitlane::Quote& q = tape[static_cast<std::size_t>(t)];
        if (!eng.on_sample(mid[static_cast<std::size_t>(t)], q.exch_us)) continue;
        if (!eng.on_quote(q.bid_px, q.ask_px, q.bid_qty, q.ask_qty, q.exch_us)) continue;
        if ((t % stride) != 0) continue;
        if (t + horizon >= n) break;
        pitlane::FactorValue v{};
        if (!eng.value(0, v) || !v.ready) continue;
        const int64_t p0 = mid[static_cast<std::size_t>(t)];
        const int64_t p1 = mid[static_cast<std::size_t>(t + horizon)];
        if (p0 <= 0) continue;
        const int64_t fwd = (p1 - p0) * 1000000 / p0;
        (void)ev.observe(v.value, fwd);
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
    return true;
}

// 75th percentile of |factor| for the chosen combo: roughly 25% of ready
// samples trigger, and the value is at least one tick to keep signals alive.
inline bool pick_threshold(const std::vector<pitlane::Quote>& tape, const std::vector<int64_t>& mid, uint8_t kind,
                           uint16_t window, int stride, int64_t& threshold_ppm, int64_t& trigger, int64_t& ready_n) {
    pitlane::FactorEngine<1, 2048> eng;
    pitlane::FactorSpec spec{};
    spec.kind = kind;
    spec.window = window;
    spec.scale = 1000000;
    if (!eng.add(spec)) return false;
    std::vector<int64_t> absv;
    absv.reserve(tape.size());
    const int n = static_cast<int>(tape.size());
    for (int t = 0; t < n; ++t) {
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

    const uint8_t kinds[] = {2, 4, 5, 6, 7, 8}; // Momentum ZScore Ema Rsi Ofi Imbalance
    const uint16_t windows[] = {4, 8, 16, 32, 64};
    const int horizons[] = {1, 4, 16};

    std::vector<GridRow> rows;
    rows.reserve(90);
    for (uint8_t k : kinds) {
        for (uint16_t w : windows) {
            for (int h : horizons) {
                GridRow r{};
                if (eval_combo(tape, mid, k, w, h, stride, r) && r.n >= 64) rows.push_back(r);
            }
        }
    }
    if (rows.empty()) {
        std::fprintf(stderr, "research: no combo reached 64 samples\n");
        return false;
    }
    std::sort(rows.begin(), rows.end(), [](const GridRow& a, const GridRow& b) {
        const int64_t ra = iabs64(a.rank_ic);
        const int64_t rb = iabs64(b.rank_ic);
        if (ra != rb) return ra > rb;
        return iabs64(a.ic) > iabs64(b.ic);
    });

    std::printf("\n=== pitlane research grid (top %d of %d) ===\n", rows.size() < 10 ? static_cast<int>(rows.size()) : 10,
                static_cast<int>(rows.size()));
    std::printf("kind  win  h     n     ic_ppm  rank_ic  hit_ppm  top_q   bottom_q\n");
    const int shown = static_cast<int>(rows.size() < 10 ? rows.size() : 10);
    for (int i = 0; i < shown; ++i) {
        const GridRow& r = rows[static_cast<std::size_t>(i)];
        std::printf("%-5u %-4u %-5d %-5lld %-7lld %-8lld %-8lld %-7lld %-7lld\n", static_cast<unsigned>(r.kind),
                    static_cast<unsigned>(r.window), r.horizon, static_cast<long long>(r.n), static_cast<long long>(r.ic),
                    static_cast<long long>(r.rank_ic), static_cast<long long>(r.hit), static_cast<long long>(r.top),
                    static_cast<long long>(r.bottom));
    }

    const GridRow& top = rows.front();
    int64_t threshold = 1;
    int64_t trigger = 0;
    int64_t ready_n = 0;
    if (!pick_threshold(tape, mid, top.kind, top.window, stride, threshold, trigger, ready_n)) {
        std::fprintf(stderr, "research: threshold pass failed\n");
        return false;
    }

    best = pitlane::StudyCfg{};
    best.kind = top.kind;
    best.window = top.window;
    best.sign = top.rank_ic < 0 ? -1 : 1;
    best.signal_ppm = threshold;
    best.horizon_bars = top.horizon;
    best.samples = top.n;
    best.ic_ppm = top.ic;
    best.rank_ic_ppm = top.rank_ic;
    best.hit_rate_ppm = top.hit;

    if (!pitlane::study_conf_save(out_path, best)) return false;
    const int64_t pct = ready_n > 0 ? trigger * 1000 / ready_n : 0;
    std::printf("best kind=%u window=%u sign=%d horizon=%d signal_ppm=%lld samples=%lld ic=%lld rank_ic=%lld\n",
                static_cast<unsigned>(best.kind), static_cast<unsigned>(best.window), static_cast<int>(best.sign),
                best.horizon_bars, static_cast<long long>(best.signal_ppm), static_cast<long long>(best.samples),
                static_cast<long long>(best.ic_ppm), static_cast<long long>(best.rank_ic_ppm));
    std::printf("threshold trigger=%lld/%lld (%.1f%%); product written to %s\n", static_cast<long long>(trigger),
                static_cast<long long>(ready_n), static_cast<double>(pct) / 10.0, out_path);
    return true;
}

} // namespace simnow_study
