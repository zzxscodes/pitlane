#pragma once

#include "pitlane/study/flow.hpp"
#include "pitlane/util/hint.hpp"
#include "pitlane/util/numeric.hpp"

#include <cstdint>

namespace pitlane {

// Integer factor engine over fixed price/quote rings (study §22).
// No heap, no floating point. Caller owns samples and quotes.
// All ratios are per-million and truncate toward zero.

enum class FactorKind : uint8_t {
    Return = 1,
    Momentum = 2,
    Volatility = 3,
    ZScore = 4,
    Ema = 5,
    Rsi = 6,
    Ofi = 7,
    Imbalance = 8
};

struct FactorSpec {
    uint32_t id{0};
    uint8_t kind{0};
    uint16_t window{0};
    int64_t scale{0};
};

struct FactorValue {
    int64_t value{0};
    int64_t stamp_us{0};
    uint8_t ready{0};
};

template <int Names = 8, int Depth = 256>
class FactorEngine {
public:
    static_assert(Names >= 1 && Names <= 32, "names");
    static_assert(Depth >= 16 && Depth <= 2048, "depth");

    // Rejects a full table, an unknown kind, or a window outside the ring.
    // IDs may repeat; slots are insertion order.
    bool add(const FactorSpec& s) noexcept {
        if (nspec_ >= Names) return false;
        if (!kind_ok(s.kind)) return false;
        if (s.window == 0 || static_cast<int>(s.window) > Depth) return false;
        spec_[nspec_++] = s;
        return true;
    }

    PITLANE_HOT PITLANE_INLINE bool on_sample(int64_t px, int64_t stamp_us) noexcept {
        if (unlikely(px <= 0)) return false;
        px_[head_] = px;
        ts_[head_] = stamp_us;
        head_ = (head_ + 1) % Depth;
        if (count_ < Depth) ++count_;
        return true;
    }

    PITLANE_HOT PITLANE_INLINE bool on_quote(int64_t bid_px, int64_t ask_px, int64_t bid_qty, int64_t ask_qty,
                                             int64_t stamp_us) noexcept {
        if (unlikely(bid_px < 0 || ask_px < 0 || bid_qty < 0 || ask_qty < 0)) return false;
        if (unlikely(bid_px > 0 && ask_px > 0 && bid_px > ask_px)) return false;
        bid_[qhead_] = bid_px;
        ask_[qhead_] = ask_px;
        bidq_[qhead_] = bid_qty;
        askq_[qhead_] = ask_qty;
        qts_[qhead_] = stamp_us;
        qhead_ = (qhead_ + 1) % Depth;
        if (qcount_ < Depth) ++qcount_;
        return true;
    }

    // Computes the factor in insertion slot `slot`. On failure out.ready is cleared.
    PITLANE_HOT bool value(int slot, FactorValue& out) const noexcept {
        if (unlikely(slot < 0 || slot >= nspec_)) return not_ready(out);
        const FactorSpec& s = spec_[slot];
        const int64_t scale = s.scale > 0 ? s.scale : 1000000;
        const int window = static_cast<int>(s.window);
        const int w = window < count_ ? window : count_;

        switch (static_cast<FactorKind>(s.kind)) {
            case FactorKind::Return: {
                if (count_ < 2) return not_ready(out);
                const int64_t p0 = px_[px_index(0)];
                const int64_t p1 = px_[px_index(1)];
                out.value = mul_div(sat_sub(p0, p1), scale, p1);
                out.stamp_us = ts_[px_index(0)];
                break;
            }
            case FactorKind::Momentum: {
                if (w < 2) return not_ready(out);
                const int64_t p0 = px_[px_index(0)];
                const int64_t pn = px_[px_index(w - 1)];
                out.value = mul_div(sat_sub(p0, pn), scale, pn);
                out.stamp_us = ts_[px_index(0)];
                break;
            }
            case FactorKind::Volatility: {
                if (w < 3) return not_ready(out);
                int64_t total = 0;
                for (int k = 0; k + 1 < w; ++k) {
                    const int64_t a = px_[px_index(k)];
                    const int64_t b = px_[px_index(k + 1)];
                    int64_t d = sat_sub(a, b);
                    if (d < 0) d = -d;
                    total = sat_add(total, mul_div(d, scale, b));
                }
                out.value = total / (w - 1);
                out.stamp_us = ts_[px_index(0)];
                break;
            }
            case FactorKind::ZScore: {
                if (w < 3) return not_ready(out);
                int64_t sum = 0;
                for (int k = 0; k < w; ++k) sum = sat_add(sum, px_[px_index(k)]);
                const int64_t mean = sum / w;
                int64_t vs = 0;
                for (int k = 0; k < w; ++k) {
                    const int64_t d = sat_sub(px_[px_index(k)], mean);
                    vs = sat_add(vs, sat_mul(d, d));
                }
                const int64_t variance = vs / w;
                if (variance <= 0) {
                    out.value = 0;
                } else {
                    const uint64_t sd = isqrt_u64(static_cast<uint64_t>(variance));
                    out.value = sd == 0 ? 0 : mul_div(sat_sub(px_[px_index(0)], mean), scale, static_cast<int64_t>(sd));
                }
                out.stamp_us = ts_[px_index(0)];
                break;
            }
            case FactorKind::Ema: {
                if (w < 2) return not_ready(out);
                const int64_t alpha = mul_div(2, scale, static_cast<int64_t>(w) + 1);
                int64_t ema = px_[px_index(w - 1)];
                for (int k = w - 2; k >= 0; --k) {
                    const int64_t x = px_[px_index(k)];
                    ema = sat_add(ema, mul_div(alpha, sat_sub(x, ema), scale));
                }
                out.value = ema;
                out.stamp_us = ts_[px_index(0)];
                break;
            }
            case FactorKind::Rsi: {
                if (w < 2) return not_ready(out);
                int64_t gain = 0;
                int64_t loss = 0;
                for (int k = 0; k + 1 < w; ++k) {
                    const int64_t d = sat_sub(px_[px_index(k)], px_[px_index(k + 1)]);
                    if (d > 0) gain = sat_add(gain, d);
                    else if (d < 0) loss = sat_add(loss, -d);
                }
                const int64_t total = sat_add(gain, loss);
                if (total == 0) out.value = sat_mul(50, scale);
                else out.value = mul_div(sat_mul(100, scale), gain, total);
                out.stamp_us = ts_[px_index(0)];
                break;
            }
            case FactorKind::Ofi: {
                if (qcount_ < 2) return not_ready(out);
                TopSnap a{};
                TopSnap b{};
                quote_at(1, a);
                quote_at(0, b);
                const int64_t ofi = ofi_step(a, b);
                const int64_t qty = sat_add(b.bid_sz, b.ask_sz);
                const int64_t denom = qty > 0 ? qty : 1;
                out.value = mul_div(ofi, scale, denom);
                out.stamp_us = qts_[qidx_at(0)];
                break;
            }
            case FactorKind::Imbalance: {
                if (qcount_ < 1) return not_ready(out);
                TopSnap b{};
                quote_at(0, b);
                const int64_t denom = sat_add(b.bid_sz, b.ask_sz);
                out.value = denom == 0 ? 0 : mul_div(sat_sub(b.bid_sz, b.ask_sz), scale, denom);
                out.stamp_us = qts_[qidx_at(0)];
                break;
            }
            default:
                return not_ready(out);
        }
        out.ready = 1;
        return true;
    }

    int count() const noexcept { return nspec_; }
    int samples() const noexcept { return count_; }
    int quotes() const noexcept { return qcount_; }

private:
    static bool kind_ok(uint8_t k) noexcept { return k >= 1 && k <= 8; }

    static bool not_ready(FactorValue& out) noexcept {
        out.ready = 0;
        return false;
    }

    static uint64_t mag64(int64_t v) noexcept {
        return v < 0 ? (~static_cast<uint64_t>(v) + 1ull) : static_cast<uint64_t>(v);
    }

    static int64_t from_mag(uint64_t m, bool neg) noexcept {
        if (!neg) {
            if (m > static_cast<uint64_t>(INT64_MAX)) return INT64_MAX;
            return static_cast<int64_t>(m);
        }
        if (m >= static_cast<uint64_t>(INT64_MAX) + 1ull) return INT64_MIN;
        return -static_cast<int64_t>(m);
    }

    static int64_t sat_add(int64_t a, int64_t b) noexcept {
        if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
        if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
        return a + b;
    }

    static int64_t sat_sub(int64_t a, int64_t b) noexcept {
        if (b > 0 && a < INT64_MIN + b) return INT64_MIN;
        if (b < 0 && a > INT64_MAX + b) return INT64_MAX;
        return a - b;
    }

    static int64_t sat_mul(int64_t a, int64_t b) noexcept {
        if (a == 0 || b == 0) return 0;
        const bool neg = (a < 0) != (b < 0);
        const uint64_t ua = mag64(a);
        const uint64_t ub = mag64(b);
        if (ua > static_cast<uint64_t>(INT64_MAX) / ub) return neg ? INT64_MIN : INT64_MAX;
        const int64_t p = static_cast<int64_t>(ua * ub);
        return neg ? -p : p;
    }

    // a*b/c with truncation toward zero, saturating instead of wrapping.
    static int64_t mul_div(int64_t a, int64_t b, int64_t c) noexcept {
        if (unlikely(c == 0)) return 0;
        const bool neg = ((a < 0) != (b < 0)) != (c < 0);
        uint64_t q = 0;
        if (!div_wide(mul_wide(mag64(a), mag64(b)), mag64(c), q)) return neg ? INT64_MIN : INT64_MAX;
        return from_mag(q, neg);
    }

    // Integer square root by binary search. No floating point.
    static uint64_t isqrt_u64(uint64_t v) noexcept {
        if (v == 0) return 0;
        uint64_t lo = 0;
        uint64_t hi = 0xffffffffull;  // sqrt(UINT64_MAX) == 2^32 - 1
        while (lo < hi) {
            const uint64_t mid = lo + (hi - lo + 1) / 2;
            if (mid <= v / mid) lo = mid;
            else hi = mid - 1;
        }
        return lo;
    }

    PITLANE_INLINE int px_index(int k) const noexcept { return (head_ - 1 - k + Depth) % Depth; }
    PITLANE_INLINE int qidx_at(int k) const noexcept { return (qhead_ - 1 - k + Depth) % Depth; }

    PITLANE_INLINE void quote_at(int k, TopSnap& s) const noexcept {
        const int i = qidx_at(k);
        s.bid_px = bid_[i];
        s.ask_px = ask_[i];
        s.bid_sz = bidq_[i];
        s.ask_sz = askq_[i];
    }

    FactorSpec spec_[Names]{};
    int nspec_{0};

    int64_t px_[Depth]{};
    int64_t ts_[Depth]{};
    int head_{0};
    int count_{0};

    int64_t bid_[Depth]{};
    int64_t ask_[Depth]{};
    int64_t bidq_[Depth]{};
    int64_t askq_[Depth]{};
    int64_t qts_[Depth]{};
    int qhead_{0};
    int qcount_{0};
};

}  // namespace pitlane
