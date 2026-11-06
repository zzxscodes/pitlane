#include "pitlane/simclock.hpp"

namespace pitlane {

int64_t SimClock::go(int64_t now) noexcept {
    if (npts > 0) return go_delay(pts, npts, go_cursor, now);
    return table_lag_us(table, minute_from_us(now), false);
}

int64_t SimClock::back(int64_t exch) noexcept {
    if (npts > 0) return back_delay(pts, npts, back_cursor, exch);
    return table_lag_us(table, minute_from_us(exch), true);
}

}  // namespace pitlane
