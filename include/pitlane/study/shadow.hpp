#pragma once

#include "pitlane/time/mag.hpp"

#include <cstdint>
#include <cstring>

namespace pitlane {

// Shadow execution accounting. A real order and a simulated order are paired
// on the same intent; the difference is kept per execution algorithm. This is
// deliberately not a second matcher: the caller runs its normal path and a
// paper venue in parallel, then hands both outcomes to shadow_note.
//
// Positive slippage means the real execution was worse than the shadow:
//   buy : real_px - sim_px
//   sell: sim_px - real_px
// All values are integer ticks or prices; no floating point enters the hot path.

enum class ShadowVerdict : uint8_t {
    Exact = 0,
    Better = 1,
    Worse = 2,
    Missed = 3,
    Extra = 4,
    BothEmpty = 5
};

struct ShadowPair {
    uint32_t algo{0};
    uint32_t tag{0};
    uint8_t side{1}; // 1 buy, 2 sell
    uint8_t pad0{0};
    uint16_t pad1{0};
    int64_t real_px{0};
    int64_t real_qty{0};
    int64_t sim_px{0};
    int64_t sim_qty{0};
    int64_t tick{1};
};

struct ShadowReport {
    uint64_t pairs{0};
    uint64_t exact{0};
    uint64_t better{0};
    uint64_t worse{0};
    uint64_t missed{0};
    uint64_t extra{0};
    uint64_t both_empty{0};
    uint64_t slip_n{0};
    int64_t slip_sum_ticks{0};
    int64_t slip_max_ticks{0};
    int64_t real_qty{0};
    int64_t sim_qty{0};
    int64_t real_notional{0};
    int64_t sim_notional{0};
    LatReport slip{};
};

inline int shadow_sign(uint8_t side) noexcept {
    return side == 2 ? -1 : 1;
}

inline int64_t shadow_abs64(int64_t v) noexcept {
    if (v == INT64_MIN) return INT64_MAX;
    return v < 0 ? -v : v;
}

inline int64_t shadow_add_sat(int64_t a, int64_t b) noexcept {
    if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
    return a + b;
}

inline int64_t shadow_mul_sat(int64_t a, int64_t b) noexcept {
    if (a == 0 || b == 0) return 0;
    if (a < 0 || b < 0) return 0; // prices and quantities are non-negative
    if (a > INT64_MAX / b) return INT64_MAX;
    return a * b;
}

class ShadowStats {
public:
    void reset() noexcept { *this = ShadowStats{}; }

    void note(const ShadowPair& p) noexcept {
        ++report_.pairs;
        const int64_t rq = p.real_qty > 0 ? p.real_qty : 0;
        const int64_t sq = p.sim_qty > 0 ? p.sim_qty : 0;
        report_.real_qty = shadow_add_sat(report_.real_qty, rq);
        report_.sim_qty = shadow_add_sat(report_.sim_qty, sq);
        report_.real_notional = shadow_add_sat(report_.real_notional, shadow_mul_sat(p.real_px, rq));
        report_.sim_notional = shadow_add_sat(report_.sim_notional, shadow_mul_sat(p.sim_px, sq));

        if (rq == 0 && sq == 0) {
            ++report_.both_empty;
            return;
        }
        if (rq == 0) {
            ++report_.missed;
            return;
        }
        if (sq == 0) {
            ++report_.extra;
            return;
        }

        const int64_t tick = p.tick > 0 ? p.tick : 1;
        const int64_t raw = p.real_px - p.sim_px;
        const int64_t signed_ticks = (raw / tick) * shadow_sign(p.side);
        const int64_t abs_ticks = shadow_abs64(signed_ticks);
        slip_.add(static_cast<uint64_t>(abs_ticks));
        report_.slip_sum_ticks = shadow_add_sat(report_.slip_sum_ticks, signed_ticks);
        if (signed_ticks > report_.slip_max_ticks) report_.slip_max_ticks = signed_ticks;
        if (signed_ticks > 0) {
            ++report_.worse;
        } else if (signed_ticks < 0) {
            ++report_.better;
        } else {
            ++report_.exact;
        }
    }

    bool report(ShadowReport& out) const noexcept {
        out = report_;
        if (!mag_report(slip_, out.slip)) {
            out.slip = LatReport{};
        }
        out.slip_n = slip_.total;
        return report_.pairs != 0;
    }

    uint64_t pairs() const noexcept { return report_.pairs; }

private:
    ShadowReport report_{};
    MagHist slip_{};
};

inline constexpr int kShadowRows = 16;
inline constexpr int kShadowName = 24;

// Fixed table for comparing execution algorithms. Each row is single-writer.
class ShadowBook {
public:
    int open(const char* name) noexcept {
        if (!name) return -1;
        for (int i = 0; i < kShadowRows; ++i) {
            if (used_[i] && std::strncmp(name_[i], name, kShadowName) == 0) return i;
        }
        for (int i = 0; i < kShadowRows; ++i) {
            if (!used_[i]) {
                used_[i] = 1;
                std::memset(name_[i], 0, kShadowName);
                std::strncpy(name_[i], name, kShadowName - 1);
                stats_[i].reset();
                return i;
            }
        }
        return -1;
    }

    bool note(int row, const ShadowPair& p) noexcept {
        if (row < 0 || row >= kShadowRows || !used_[row]) return false;
        stats_[row].note(p);
        return true;
    }

    bool report(int row, ShadowReport& out) const noexcept {
        if (row < 0 || row >= kShadowRows || !used_[row]) return false;
        return stats_[row].report(out);
    }

    const char* name(int row) const noexcept {
        if (row < 0 || row >= kShadowRows || !used_[row]) return nullptr;
        return name_[row];
    }

    int rows() const noexcept { return kShadowRows; }
    void reset() noexcept { *this = ShadowBook{}; }

private:
    ShadowStats stats_[kShadowRows]{};
    char name_[kShadowRows][kShadowName]{};
    uint8_t used_[kShadowRows]{};
};

} // namespace pitlane
