#pragma once

#include "pitlane/util/hint.hpp"
#include "pitlane/util/numeric.hpp"

#include <cstdint>

namespace pitlane {

// Recover per-price lots from two cumulative volume/turnover snapshots (integer only).

struct CumSnap {
    int64_t vol{0};
    int64_t turn{0};  // turnover in money units matching mult
    int64_t last{0};
    int64_t bid1{0};
    int64_t ask1{0};
};

namespace detail {

PITLANE_INLINE int64_t snap_tick(int64_t px, int64_t tick) noexcept {
    if (tick <= 0) return px;
    return round_nearest_to(px, tick);
}

PITLANE_INLINE bool push_px(int64_t* px, int64_t* lots, int& n, int cap, int64_t p, int64_t q) noexcept {
    if (q <= 0) return true;
    for (int i = 0; i < n; ++i) {
        if (px[i] != p) continue;
        lots[i] += q;
        return true;
    }
    if (n >= cap) return false;
    px[n] = p;
    lots[n] = q;
    ++n;
    return true;
}

}  // namespace detail

// Returns written price levels, or -1 on overflow/bad args. flat_venue books all dv at last.
inline int infer_lots(const CumSnap& prev, const CumSnap& cur, int64_t tick, int64_t mult, bool flat_venue,
                      int64_t* px, int64_t* lots, int cap) noexcept {
    if (unlikely(!px || !lots || cap <= 0 || tick <= 0 || mult <= 0)) return -1;
    const int64_t dv = cur.vol - prev.vol;
    if (dv <= 0) return 0;
    int n = 0;
    if (flat_venue) {
        return detail::push_px(px, lots, n, cap, cur.last, dv) ? n : -1;
    }
    const int64_t dturn = cur.turn - prev.turn;
    if (dturn < 0) return 0;
    const int64_t notional = dturn / mult;
    const int64_t tol = tick / 10;
    if (dv == 1) {
        int64_t p = cur.last;
        const int64_t diff = notional >= cur.last ? notional - cur.last : cur.last - notional;
        if (diff >= tol) p = detail::snap_tick(notional, tick);
        return detail::push_px(px, lots, n, cap, p, 1) ? n : -1;
    }
    const int64_t rest = (notional - cur.last) / (dv - 1);
    int64_t lo = cur.bid1 > 0 ? (rest < cur.bid1 ? cur.bid1 : rest) : rest;
    int64_t hi = cur.ask1 > 0 ? (rest > cur.ask1 ? cur.ask1 : rest) : rest;
    if (cur.last < lo) lo = cur.last;
    if (cur.last > hi) hi = cur.last;
    lo = detail::snap_tick(lo, tick);
    hi = detail::snap_tick(hi, tick);
    if (hi <= lo) return detail::push_px(px, lots, n, cap, cur.last, dv) ? n : -1;
    // n_lo ≈ (hi*dv - notional) / (hi - lo)
    const int64_t span = hi - lo;
    int64_t n_lo = (hi * dv - notional) / span;
    if (n_lo < 0) n_lo = 0;
    if (n_lo > dv) n_lo = dv;
    if (!detail::push_px(px, lots, n, cap, lo, n_lo)) return -1;
    if (!detail::push_px(px, lots, n, cap, hi, dv - n_lo)) return -1;
    int64_t got = lo * n_lo + hi * (dv - n_lo);
    while (got + tol < notional && n_lo > 0) {
        // move one lot from lo toward hi by one tick
        for (int i = 0; i < n; ++i) {
            if (px[i] != lo || lots[i] <= 0) continue;
            --lots[i];
            break;
        }
        lo += tick;
        if (!detail::push_px(px, lots, n, cap, lo, 1)) return -1;
        --n_lo;
        got += tick;
    }
    // drop empty rows
    int w = 0;
    for (int i = 0; i < n; ++i) {
        if (lots[i] <= 0) continue;
        px[w] = px[i];
        lots[w] = lots[i];
        ++w;
    }
    return w;
}

}  // namespace pitlane
