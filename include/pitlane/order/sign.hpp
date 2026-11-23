#pragma once

#include <cstdint>

namespace pitlane {

// Pass and reject counts for one order. The library does not send it.
// A reject stays until the caller resets the slip.

struct SignSlip {
    uint8_t checked{0};
    uint8_t rejected{0};
    uint8_t need{1};
};

inline bool sign_ready(const SignSlip& slip) noexcept { return slip.rejected == 0 && slip.checked >= slip.need; }

inline bool sign_pass(SignSlip& slip) noexcept {
    if (slip.rejected != 0 || slip.checked == 255) return false;
    ++slip.checked;
    return true;
}

inline bool sign_reject(SignSlip& slip) noexcept {
    if (slip.rejected == 255) return false;
    ++slip.rejected;
    return true;
}

inline void sign_reset(SignSlip& slip) noexcept {
    slip.checked = 0;
    slip.rejected = 0;
}

// Multi-module convention: sign_begin(need) → each module sign_pass/sign_reject → sign_ready.
// Typical need=3 for global / account / lane. Reject sticks until sign_reset/sign_begin.

inline bool sign_begin(SignSlip& slip, uint8_t need) noexcept {
    if (need == 0) return false;
    sign_reset(slip);
    slip.need = need;
    return true;
}

// Opening pre-holds quantity and one cancel credit. Closing holds neither.
// The caller keeps OpenHold. A timed-out open returns both. A cancel that did not reach the venue returns the credit.

struct OpenQuota {
    int64_t lots{0};
    int cancels{0};
};

struct OpenHold {
    int64_t qty{0};
    int64_t done{0};
    uint8_t opening{0};
    uint8_t cancel_held{0};
    uint8_t on{0};
};

inline bool hold_reserve(OpenQuota& quota, OpenHold& hold, bool opening, int64_t qty) noexcept {
    if (hold.on || qty <= 0) return false;
    if (opening) {
        if (quota.lots < qty || quota.cancels < 1) return false;
        quota.lots -= qty;
        --quota.cancels;
        hold.cancel_held = 1;
        hold.opening = 1;
    } else {
        hold.cancel_held = 0;
        hold.opening = 0;
    }
    hold.qty = qty;
    hold.done = 0;
    hold.on = 1;
    return true;
}

inline bool hold_cancel(OpenQuota& quota, OpenHold& hold, int64_t done, bool rejected_here) noexcept {
    if (!hold.on || !hold.opening || done < hold.done || done > hold.qty) return false;
    const int64_t rest = hold.qty - done;
    const bool give = rejected_here && hold.cancel_held != 0;
    if (quota.lots > INT64_MAX - rest) return false;
    if (give && quota.cancels == INT32_MAX) return false;
    quota.lots += rest;
    if (give) ++quota.cancels;
    hold.cancel_held = 0;
    hold.done = done;
    hold.on = 0;
    return true;
}

inline bool hold_done(OpenQuota& quota, OpenHold& hold, int64_t done, bool outside) noexcept {
    if (!hold.on || done < hold.done || done > hold.qty) return false;
    if (done < hold.qty) {
        hold.done = done;
        return true;
    }
    const bool give = !outside && hold.opening && hold.cancel_held;
    if (give && quota.cancels == INT32_MAX) return false;
    if (give) ++quota.cancels;
    hold.cancel_held = 0;
    hold.done = done;
    hold.on = 0;
    return true;
}

inline bool hold_late(OpenQuota& quota, OpenHold& hold, int64_t done) noexcept {
    if (!hold.on || done < hold.done || done > hold.qty) return false;
    if (!hold.opening) {
        hold.done = done;
        hold.on = 0;
        return true;
    }
    const int64_t rest = hold.qty - done;
    const bool give = hold.cancel_held != 0;
    if (quota.lots > INT64_MAX - rest) return false;
    if (give && quota.cancels == INT32_MAX) return false;
    quota.lots += rest;
    if (give) ++quota.cancels;
    hold.cancel_held = 0;
    hold.done = done;
    hold.on = 0;
    return true;
}

inline bool hold_cancel_late(OpenQuota& quota, OpenHold& hold) noexcept {
    if (!hold.on || !hold.cancel_held) return false;
    if (quota.cancels == INT32_MAX) return false;
    ++quota.cancels;
    hold.cancel_held = 0;
    return true;
}

}  // namespace pitlane
