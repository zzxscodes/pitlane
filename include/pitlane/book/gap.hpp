#pragma once

#include <cmath>
#include <cstdint>

namespace pitlane {

// Quantity sitting ahead of a simulated resting order.
// Prints reduce it. A later size change subtracts the prints already counted
// so the same trade is not removed twice. Overshoot below zero becomes a fill
// only after it rounds to at least one lot.

struct Gap {
    double ahead{0};
    double bucket{0};
};

inline void gap_join(Gap& g, int64_t shown) noexcept {
    g.ahead = double(shown);
    g.bucket = 0;
}

inline int64_t gap_overshoot(Gap& g, int64_t lot, int64_t leaves) noexcept {
    if (g.ahead >= 0 || lot <= 0 || leaves <= 0) return 0;
    const int64_t lots = int64_t(std::llround(-g.ahead / double(lot)));
    if (lots <= 0) return 0;
    int64_t q = lots * lot;
    if (q > leaves) q = leaves;
    g.ahead = 0;
    return q;
}

inline int64_t gap_print(Gap& g, int64_t qty, int64_t lot, int64_t leaves) noexcept {
    g.ahead -= double(qty);
    g.bucket += double(qty);
    return gap_overshoot(g, lot, leaves);
}

// prev/now are displayed sizes at this price. power skews cancels toward the heavier side.
inline int64_t gap_resize(Gap& g, int64_t prev, int64_t now, double power, int64_t lot, int64_t leaves) noexcept {
    double chg = double(prev) - double(now) - g.bucket;
    g.bucket = 0;
    if (chg < 0) {
        if (g.ahead > double(now)) g.ahead = double(now);
        return gap_overshoot(g, lot, leaves);
    }
    double front = g.ahead;
    double back = double(prev) - front;
    if (back < 0) back = 0;
    const double pf = std::pow(front, power);
    const double pb = std::pow(back, power);
    const double den = pf + pb;
    double on_back = den == 0.0 ? 1.0 : pb / den;
    if (!std::isfinite(on_back)) on_back = 1.0;
    double est = front - (1.0 - on_back) * chg;
    const double back_left = back - on_back * chg;
    if (back_left < 0) est += back_left;
    if (est > double(now)) est = double(now);
    g.ahead = est;
    return gap_overshoot(g, lot, leaves);
}

}  // namespace pitlane
