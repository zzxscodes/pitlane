#pragma once

#include <cstdint>

namespace pitlane {

// Settings shared by the control plane, the risk guard, and the example loader.
// A zero limit is off. Quantities are board lots, prices are integer ticks.

struct OpsPolicy {
    int64_t max_manual_qty{10};
    int64_t max_manual_notional{500000000};
    int32_t max_manual_per_min{30};
    int64_t max_daily_loss{0};
    int64_t max_drawdown_ppm{0};
    int32_t max_reject_ppm{300000};
    int64_t stale_tick_us{5000000};
};

}  // namespace pitlane
