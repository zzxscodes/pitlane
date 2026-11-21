#pragma once

#include "pitlane/desk/broker.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Positive limits: take the stricter (smaller). Zero means that axis is off.

PITLANE_INLINE int64_t risk_min_pos(int64_t a, int64_t b) noexcept {
    if (a <= 0) return b;
    if (b <= 0) return a;
    return a < b ? a : b;
}

PITLANE_INLINE int risk_min_pos_i(int a, int b) noexcept {
    if (a <= 0) return b;
    if (b <= 0) return a;
    return a < b ? a : b;
}

PITLANE_INLINE RiskCaps risk_merge(const RiskCaps& global, const RiskCaps& acct) noexcept {
    RiskCaps out{};
    out.max_qty = risk_min_pos(global.max_qty, acct.max_qty);
    out.max_notional = risk_min_pos(global.max_notional, acct.max_notional);
    out.max_live = risk_min_pos_i(global.max_live, acct.max_live);
    return out;
}

// Fixed account → RiskCaps table. Miss is fail-closed (get returns false).

template <int Cap>
class AcctRiskTab {
public:
    static_assert(Cap >= 1 && Cap <= 1024, "cap");

    bool put(uint32_t acct, const RiskCaps& caps) noexcept {
        const int i = find(acct);
        if (i >= 0) {
            row_[i].caps = caps;
            return true;
        }
        for (int j = 0; j < Cap; ++j) {
            if (row_[j].used) continue;
            row_[j].acct = acct;
            row_[j].caps = caps;
            row_[j].used = 1;
            return true;
        }
        return false;
    }

    bool get(uint32_t acct, RiskCaps& out) const noexcept {
        const int i = find(acct);
        if (i < 0) return false;
        out = row_[i].caps;
        return true;
    }

    // Miss fails closed: returns false and leaves out untouched.
    bool merged(uint32_t acct, const RiskCaps& global, RiskCaps& out) const noexcept {
        RiskCaps acct_caps{};
        if (!get(acct, acct_caps)) return false;
        out = risk_merge(global, acct_caps);
        return true;
    }

private:
    struct Row {
        uint32_t acct{0};
        RiskCaps caps{};
        uint8_t used{0};
    };

    int find(uint32_t acct) const noexcept {
        for (int i = 0; i < Cap; ++i) {
            if (row_[i].used && row_[i].acct == acct) return i;
        }
        return -1;
    }

    Row row_[Cap]{};
};

}  // namespace pitlane
