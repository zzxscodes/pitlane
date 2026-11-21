// Event-driven historical backtester over a recorded quote tape.
// Cold path (research/backtest only): fixed capacity, no heap, no exceptions.
// Prices and cash are integers; metrics that need roots use long double.

#pragma once

#include "pitlane/desk/types.hpp"
#include "pitlane/study/factor.hpp"

#include <cmath>
#include <cstdint>

namespace pitlane {

struct BtPoint {
    int64_t ts_us{0};
    int64_t equity{0};
    int64_t dd_ppm{0};
};

struct BtCfg {
    int64_t cash{1000000};  // initial cash, currency units
    int64_t qty{1};         // size of one target change
    int64_t fee_ppm{20};    // per-side fee in ppm of notional
    int64_t min_fee{0};
    int64_t slip_ticks{0};  // extra ticks beyond the touch
    int64_t tick{1};
    int64_t threshold_ppm{10};
    int64_t cooldown_us{0};
    int8_t sign{1};
    uint8_t kind{2};  // FactorKind
    uint16_t window{16};
    int stride{1};  // evaluate every Nth tick, every tick still feeds the factor
};

struct BtMetrics {
    int64_t points{0};
    int64_t trades{0};
    int64_t round_trips{0};
    int64_t wins{0};
    int64_t gross_pnl{0};
    int64_t fees{0};
    int64_t net_pnl{0};
    int64_t turnover{0};
    int64_t return_ppm{0};
    int64_t annual_ppm{0};
    int64_t sharpe_ppm{0};  // Sharpe * 1e6
    int64_t max_dd_ppm{0};
    int64_t win_ppm{0};
    int64_t exposure_ppm{0};
    int64_t avg_trade_pnl{0};
    int64_t span_us{0};
    int64_t final_equity{0};
    int64_t peak_equity{0};
};

// Trades a single instrument from a factor signal: target is +qty when
// sign*factor >= threshold, -qty when <= -threshold, flat otherwise. Fills at
// the touch with optional slippage and per-side fees. Equity is cash plus the
// marked position, sampled at most Cap points.
template <int Cap = 1024>
class Backtester {
public:
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "backtest capacity must be a power of two");

    bool run(const Quote* tape, int n, const BtCfg& cfg, BtMetrics& out) noexcept {
        out = BtMetrics{};
        pn_ = 0;
        if (!tape || n < 2) return false;
        if (cfg.cash <= 0 || cfg.qty <= 0 || cfg.window < 1 || cfg.window > 256) return false;
        if (cfg.kind < 1 || cfg.kind > 8) return false;
        if (cfg.stride < 1) return false;

        FactorEngine<1, 256> eng;
        FactorSpec spec{};
        spec.kind = cfg.kind;
        spec.window = cfg.window;
        spec.scale = 1000000;
        if (!eng.add(spec)) return false;

        int64_t cash = cfg.cash;
        int64_t pos = 0;
        int64_t entry_px = 0;
        int64_t realized = 0;
        int64_t fees = 0;
        int64_t turnover = 0;
        int64_t trades = 0;
        int64_t round_trips = 0;
        int64_t wins = 0;
        int64_t peak = cfg.cash;
        int64_t max_dd = 0;
        int64_t first_ts = 0;
        int64_t last_ts = 0;
        int64_t last_sig_us = INT64_MIN / 4;
        int64_t prev_eq = cfg.cash;
        bool have_prev = false;
        bool have_ts = false;
        long double ret_sum = 0;
        long double ret_sq = 0;
        int ret_n = 0;
        int evaluated = 0;
        int exposed = 0;
        int ticks = 0;
        int64_t last_eq = cfg.cash;

        // Reserve one slot for the final point.
        int rec_stride = (n + Cap - 2) / (Cap - 1);
        if (rec_stride < 1) rec_stride = 1;

        for (int i = 0; i < n; ++i) {
            const Quote& q = tape[i];
            int64_t mid = q.last_px;
            if (mid <= 0) mid = (q.bid_px > 0 && q.ask_px > 0) ? (q.bid_px + q.ask_px) / 2 : 0;
            if (mid <= 0) continue;
            const int64_t ts = q.local_us;

            (void)eng.on_sample(mid, ts);
            (void)eng.on_quote(q.bid_px, q.ask_px, q.bid_qty, q.ask_qty, ts);
            if (!have_ts) {
                first_ts = ts;
                have_ts = true;
            }
            last_ts = ts;
            ++ticks;

            if (i % cfg.stride == 0) {
                FactorValue fv{};
                if (eng.value(0, fv) && fv.ready) {
                    ++evaluated;
                    const int64_t dir = fv.value * static_cast<int64_t>(cfg.sign);
                    const int8_t target =
                        dir >= cfg.threshold_ppm ? 1 : (dir <= -cfg.threshold_ppm ? -1 : 0);
                    const int64_t tpos = target == 0 ? 0 : (target > 0 ? cfg.qty : -cfg.qty);
                    if (tpos != pos && ts - last_sig_us >= cfg.cooldown_us) {
                        const int64_t delta = tpos - pos;
                        int64_t px = delta > 0 ? q.ask_px : q.bid_px;
                        if (px <= 0) px = mid;
                        px += (delta > 0 ? 1 : -1) * cfg.slip_ticks * cfg.tick;
                        if (px <= 0) px = mid;
                        const int64_t absq = delta > 0 ? delta : -delta;
                        const int64_t notional = absq * px;
                        int64_t fee = (notional * cfg.fee_ppm + 999999) / 1000000;
                        if (fee < cfg.min_fee) fee = cfg.min_fee;

                        const int64_t oldabs = pos > 0 ? pos : -pos;
                        const bool same_dir = (pos > 0 && delta > 0) || (pos < 0 && delta < 0);
                        if (!same_dir && oldabs > 0) {
                            const int64_t closeabs = absq < oldabs ? absq : oldabs;
                            const int64_t pnl = (px - entry_px) * closeabs * (pos > 0 ? 1 : -1);
                            realized += pnl;
                            if (absq >= oldabs) {
                                ++round_trips;
                                if (pnl > 0) ++wins;
                            }
                        }
                        if (same_dir) {
                            const int64_t newabs = oldabs + absq;
                            entry_px = (entry_px * oldabs + px * absq) / newabs;
                        } else if (absq > oldabs) {
                            entry_px = px;
                        } else if (absq == oldabs) {
                            entry_px = 0;
                        }
                        pos += delta;
                        cash -= delta * px;
                        cash -= fee;
                        fees += fee;
                        turnover += notional;
                        ++trades;
                        last_sig_us = ts;
                    }
                }
            }

            if (pos != 0) ++exposed;
            const int64_t eq = cash + pos * mid;
            last_eq = eq;
            if (eq > peak) peak = eq;
            if (peak > 0) {
                const int64_t dd = (peak - eq) * 1000000 / peak;
                if (dd > max_dd) max_dd = dd;
            }

            if ((i % rec_stride == 0 || i == n - 1) && pn_ < Cap) {
                if (pn_ == 0 || pts_[pn_ - 1].ts_us != ts) {
                    const int64_t dd = peak > 0 ? (peak - eq) * 1000000 / peak : 0;
                    pts_[pn_++] = BtPoint{ts, eq, dd};
                    if (have_prev && prev_eq > 0) {
                        const long double r =
                            static_cast<long double>(eq - prev_eq) / static_cast<long double>(prev_eq);
                        ret_sum += r;
                        ret_sq += r * r;
                        ++ret_n;
                    }
                    prev_eq = eq;
                    have_prev = true;
                }
            }
        }

        if (!have_ts || pn_ < 2) return false;

        const int64_t span = last_ts > first_ts ? last_ts - first_ts : 0;
        const int64_t final_eq = last_eq;
        const int64_t net = final_eq - cfg.cash;

        out.points = pn_;
        out.trades = trades;
        out.round_trips = round_trips;
        out.wins = wins;
        out.fees = fees;
        out.net_pnl = net;
        out.gross_pnl = net + fees;
        out.turnover = turnover;
        out.final_equity = final_eq;
        out.peak_equity = peak;
        out.max_dd_ppm = max_dd;
        out.span_us = span;
        out.return_ppm = net * 1000000 / cfg.cash;
        out.avg_trade_pnl = trades > 0 ? realized / trades : 0;
        out.exposure_ppm = ticks > 0 ? static_cast<int64_t>(exposed) * 1000000 / ticks : 0;
        out.win_ppm = round_trips > 0 ? wins * 1000000 / round_trips : 0;
        if (span > 0) {
            const long double annual = static_cast<long double>(net) * 1000000.0L * 31536000.0e6L /
                                       (static_cast<long double>(cfg.cash) * static_cast<long double>(span));
            out.annual_ppm = static_cast<int64_t>(annual);
        }
        if (ret_n > 0 && span > 0) {
            const long double mean = ret_sum / static_cast<long double>(ret_n);
            long double var = ret_sq / static_cast<long double>(ret_n) - mean * mean;
            if (var < 0) var = 0;
            if (var > 0) {
                const long double sd = std::sqrt(static_cast<double>(var));
                const long double per_year = static_cast<long double>(ret_n) * 31536000.0e6L /
                                             static_cast<long double>(span);
                const long double sharpe = mean / sd * std::sqrt(static_cast<double>(per_year));
                out.sharpe_ppm = static_cast<int64_t>(sharpe * 1000000.0L);
            }
        }
        return true;
    }

    int curve(BtPoint* out, int cap) const noexcept {
        if (!out || cap <= 0) return 0;
        const int m = pn_ < cap ? pn_ : cap;
        for (int i = 0; i < m; ++i) out[i] = pts_[i];
        return m;
    }

    int curve_points() const noexcept { return pn_; }

private:
    BtPoint pts_[Cap]{};
    int pn_{0};
};

}  // namespace pitlane
