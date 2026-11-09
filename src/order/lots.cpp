#include "pitlane/order/lots.hpp"

namespace pitlane {
namespace {

int64_t clip_lot(int64_t n, int64_t max_lot, int64_t min_lot) noexcept {
    if (n <= 0) return 0;
    if (max_lot > 0 && n > max_lot) n = max_lot;
    if (min_lot > 0 && n < min_lot) return 0;
    return n;
}

int push_slice(LotSlice* out, int cap, int n, Off off, int64_t qty) noexcept {
    if (qty <= 0 || n >= cap || !out) return n;
    out[n] = LotSlice{off, qty};
    return n + 1;
}

}  // namespace

int split_lots(int64_t want, LotPos pos, LotPend pend, int64_t max_lot, int64_t min_lot, SplitMode mode, LotSlice* out,
               int cap) noexcept {
    if (want <= 0 || !out || cap <= 0) return 0;
    if (mode != SplitMode::TodayThenYdayThenOpen && mode != SplitMode::CloseOnly && mode != SplitMode::YdayThenStop)
        return -1;
    int n = 0;
    int64_t left = want;
    if (mode != SplitMode::YdayThenStop) {
        const int64_t td_free = pos.today > pend.close_today ? pos.today - pend.close_today : 0;
        const int64_t td = clip_lot(td_free < left ? td_free : left, max_lot, min_lot);
        if (td > 0) {
            n = push_slice(out, cap, n, Off::CloseToday, td);
            left -= td;
        }
    }
    const int64_t yday = pos.total > pos.today ? pos.total - pos.today : 0;
    const int64_t yd_free = yday > pend.close_yday ? yday - pend.close_yday : 0;
    const int64_t yd = clip_lot(yd_free < left ? yd_free : left, max_lot, min_lot);
    if (yd > 0) {
        n = push_slice(out, cap, n, Off::CloseYday, yd);
        left -= yd;
    }
    if (mode == SplitMode::TodayThenYdayThenOpen) {
        while (left > 0 && n < cap) {
            const int64_t chunk = clip_lot(left, max_lot, min_lot);
            if (chunk <= 0) break;
            n = push_slice(out, cap, n, Off::Open, chunk);
            left -= chunk;
        }
    }
    return n;
}

}  // namespace pitlane
