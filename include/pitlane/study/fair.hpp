#pragma once

#include <cstdint>

namespace pitlane {

// Size-weighted fair, book bias, and trade-pressure score (notes calc §19).
// All Q-format integers: scores are in parts-per-million of the spread unless noted.

inline int64_t fair_from_top(int64_t bid_px, int64_t bid_qty, int64_t ask_px, int64_t ask_qty) noexcept {
    if (bid_px <= 0 || ask_px <= 0 || ask_px < bid_px) return 0;
    if (bid_qty < 0 || ask_qty < 0) return 0;
    const int64_t den = bid_qty + ask_qty;
    if (den <= 0) return (bid_px + ask_px) / 2;
    // fair = (ask*bid_qty + bid*ask_qty) / (bid_qty+ask_qty)
    if (ask_px > INT64_MAX / (bid_qty + 1) || bid_px > INT64_MAX / (ask_qty + 1)) return (bid_px + ask_px) / 2;
    return (ask_px * bid_qty + bid_px * ask_qty) / den;
}

// Positive bias favors the bid side (more ask size → price pressure up). ppm of mid.
inline int64_t book_bias_ppm(int64_t bid_qty, int64_t ask_qty) noexcept {
    if (bid_qty < 0 || ask_qty < 0) return 0;
    const int64_t den = bid_qty + ask_qty;
    if (den <= 0) return 0;
    return (ask_qty - bid_qty) * 1000000 / den;
}

struct PressureEma {
    int64_t value{0};  // ppm
    int64_t alpha_ppm{100000};  // 0.1 default
    uint8_t on{0};
};

inline bool pressure_open(PressureEma& p, int64_t alpha_ppm) noexcept {
    if (alpha_ppm <= 0 || alpha_ppm > 1000000) return false;
    p.value = 0;
    p.alpha_ppm = alpha_ppm;
    p.on = 1;
    return true;
}

// Sample (trade_px - mid) / spread in ppm, then EMA.
inline bool pressure_sample(PressureEma& p, int64_t trade_px, int64_t mid, int64_t spread) noexcept {
    if (!p.on || spread <= 0 || mid <= 0) return false;
    const int64_t raw = (trade_px - mid) * 1000000 / spread;
    const int64_t a = p.alpha_ppm;
    p.value = (raw * a + p.value * (1000000 - a)) / 1000000;
    return true;
}

inline int64_t fair_score(int64_t bias_ppm, int64_t pressure_ppm, int64_t bias_w = 1, int64_t press_w = 1) noexcept {
    if (bias_w < 0 || press_w < 0) return 0;
    if (bias_w > 0 && bias_ppm > INT64_MAX / bias_w) return INT64_MAX;
    if (press_w > 0 && pressure_ppm > INT64_MAX / press_w) return INT64_MAX;
    if (bias_w > 0 && bias_ppm < INT64_MIN / bias_w) return INT64_MIN;
    if (press_w > 0 && pressure_ppm < INT64_MIN / press_w) return INT64_MIN;
    const int64_t a = bias_ppm * bias_w;
    const int64_t b = pressure_ppm * press_w;
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return b > 0 ? INT64_MAX : INT64_MIN;
    return a + b;
}

}  // namespace pitlane
