#pragma once

// Book-only prices for a parent and a chase. No clock and no account state.

#include "pitlane/util/numeric.hpp"

#include <cstdint>

namespace pitlane {

struct QuoteTop {
    int64_t bid[3]{};
    int64_t ask[3]{};
    int64_t bid_qty{0};
    int64_t ask_qty{0};
    int64_t last{0};
    int64_t hint{0};
};

inline int64_t align_half(int64_t px, int64_t tick) noexcept {
    if (px <= 0 || tick <= 0) return px > 0 ? px : 0;
    const int64_t steps = px / tick;
    const int64_t rem = px % tick;
    const int64_t up = (rem > (INT64_MAX / 2) || rem * 2 >= tick) ? steps + 1 : steps;
    if (up > INT64_MAX / tick) return INT64_MAX;
    return up * tick;
}

// Level 0 is the touch VWAP (or the live side, or the mid when sizes are missing).
// A positive level reads that rung on the side the order would take, then aligns to the tick.
inline int64_t price_at_quote_vwap(bool buy, int level, const QuoteTop& q, int64_t tick) noexcept {
    int64_t raw = 0;
    if (level <= 0) {
        const bool hb = q.bid[0] > 0;
        const bool ha = q.ask[0] > 0;
        if (hb && ha && q.bid_qty > 0 && q.ask_qty > 0) {
            if (q.bid_qty > INT64_MAX - q.ask_qty) return 0;
            const int64_t den = q.bid_qty + q.ask_qty;
            if (cmp_prod(q.bid[0], q.bid_qty, INT64_MAX, 1) > 0) return 0;
            if (cmp_prod(q.ask[0], q.ask_qty, INT64_MAX, 1) > 0) return 0;
            const int64_t num = q.bid[0] * q.bid_qty + q.ask[0] * q.ask_qty;
            if (num < 0) return 0;
            raw = num / den;
        } else if (hb && ha) {
            raw = (q.bid[0] + q.ask[0]) / 2;
        } else if (hb) {
            raw = q.bid[0];
        } else if (ha) {
            raw = q.ask[0];
        }
    } else if (level <= 3) {
        raw = buy ? q.ask[level - 1] : q.bid[level - 1];
    }
    if (raw <= 0) return 0;
    return align_half(raw, tick);
}

// mode: 0 opposite, 1 own, 2 mid, 3 last, 4 hint. A dead side flips when reroute is set.
// A short sale uses last except when joining the own side.
inline int64_t pre_px(bool buy, int mode, const QuoteTop& q, bool reroute, bool short_sale) noexcept {
    if (short_sale && mode != 1) return q.last > 0 ? q.last : 0;
    if (mode == 2) {
        if (q.bid[0] > 0 && q.ask[0] > 0) return (q.bid[0] + q.ask[0]) / 2;
        if (q.bid[0] > 0) return q.bid[0];
        return q.ask[0] > 0 ? q.ask[0] : 0;
    }
    if (mode == 3) return q.last > 0 ? q.last : 0;
    if (mode == 4) return q.hint > 0 ? q.hint : 0;
    const bool opp = mode == 0;
    const int64_t px = opp ? (buy ? q.ask[0] : q.bid[0]) : (buy ? q.bid[0] : q.ask[0]);
    if (px > 0 || !reroute) return px > 0 ? px : 0;
    const int64_t flip = opp ? (buy ? q.bid[0] : q.ask[0]) : (buy ? q.ask[0] : q.bid[0]);
    return flip > 0 ? flip : 0;
}

// Passive price stepped inside the spread, and only while the step stays under half the spread.
inline int64_t post_px(bool buy, int64_t bid, int64_t ask, int64_t tick, int steps) noexcept {
    if (bid <= 0 || ask <= bid || tick <= 0 || steps <= 0) return buy ? bid : ask;
    if (steps > 1000000) steps = 1000000;
    const int64_t span = ask - bid;
    int n = 0;
    if (steps <= INT64_MAX / tick && steps * tick * 2 < span) n = steps;
    int64_t raw = buy ? bid : ask;
    if (n > 0) {
        const int64_t move = static_cast<int64_t>(n) * tick;
        if (buy) {
            if (raw > INT64_MAX - move) return 0;
            raw += move;
        } else {
            raw = raw > move ? raw - move : 0;
        }
    }
    return align_half(raw, tick);
}

// Chase from the opposite top, the own top, or the hint. Positive steps are more aggressive.
inline int64_t chasing_px(bool buy, int mode, const QuoteTop& q, int64_t tick, int steps) noexcept {
    const int64_t base = mode == 0 ? (buy ? q.ask[0] : q.bid[0]) : mode == 1 ? (buy ? q.bid[0] : q.ask[0]) : q.hint;
    if (base <= 0 || tick <= 0) return 0;
    if (steps > 1000000) steps = 1000000;
    if (steps < -1000000) steps = -1000000;
    const int signed_steps = buy ? steps : -steps;
    int64_t raw = base;
    if (signed_steps > 0) {
        if (tick > INT64_MAX / signed_steps) return 0;
        const int64_t move = static_cast<int64_t>(signed_steps) * tick;
        if (raw > INT64_MAX - move) return 0;
        raw += move;
    } else if (signed_steps < 0) {
        const int mag_steps = -signed_steps;
        if (tick > INT64_MAX / mag_steps) return 0;
        const int64_t move = static_cast<int64_t>(mag_steps) * tick;
        raw = raw > move ? raw - move : 0;
    }
    return align_half(raw, tick);
}

}  // namespace pitlane
