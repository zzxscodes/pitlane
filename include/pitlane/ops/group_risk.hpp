#pragma once

// Portfolio-level risk aggregation. One pass over the account books combines
// net quantity with a mark price into a signed notional per account, then per
// group. Groups are checked at 1 Hz, never on the send path, so this class may
// keep plain arrays and a full sweep.
//
// A group limit of zero is off. `breach` keeps the first reason bits until the
// next clear so an operator sees what tripped even after a manual flatten.

#include <cstdint>

namespace pitlane {

struct GroupCaps {
    int64_t max_gross{0};
    int64_t max_net{0};
    int64_t max_acct{0};
};

struct GroupRow {
    int64_t gross{0};
    int64_t net{0};
    int64_t largest{0};
    int32_t accounts{0};
    uint8_t breach{0};
};

enum : uint8_t {
    kGroupGross = 1u << 0,
    kGroupNet = 1u << 1,
    kGroupAcct = 1u << 2,
    kGroupHalted = 1u << 3,
};

class GroupRiskBook {
public:
    static constexpr uint32_t kGroups = 8;
    static constexpr uint32_t kAccounts = 32;

    void clear(int64_t now_us) noexcept {
        for (uint32_t g = 0; g < kGroups; ++g) {
            rows_[g] = GroupRow{};
            for (uint32_t a = 0; a < kAccounts; ++a) notional_[g][a] = 0;
        }
        now_us_ = now_us;
    }

    bool set_caps(uint32_t group, const GroupCaps& caps) noexcept {
        if (group >= kGroups) return false;
        caps_[group] = caps;
        return true;
    }

    const GroupCaps& caps(uint32_t group) const noexcept { return caps_[group < kGroups ? group : 0]; }

    void halt(uint32_t group, bool on) noexcept {
        if (group >= kGroups) return;
        if (on) {
            rows_[group].breach |= kGroupHalted;
        } else {
            rows_[group].breach &= static_cast<uint8_t>(~kGroupHalted);
        }
    }

    // Adds one account book line. `qty` is signed, `px` is the mark; a zero or
    // negative price contributes nothing. Returns false for an out-of-range
    // group or account.
    bool add(uint32_t group, uint32_t acct, int64_t qty, int64_t px) noexcept {
        if (group >= kGroups || acct >= kAccounts) return false;
        if (px <= 0 || qty == 0) return true;
        const int64_t v = mul_sat(qty, px);
        notional_[group][acct] = sat_add(notional_[group][acct], v);
        return true;
    }

    // Folds every account line into the group rows and sets the breach bits.
    void evaluate(int64_t now_us) noexcept {
        now_us_ = now_us;
        for (uint32_t g = 0; g < kGroups; ++g) {
            GroupRow row = rows_[g];
            row.gross = 0;
            row.net = 0;
            row.largest = 0;
            row.accounts = 0;
            row.breach = static_cast<uint8_t>(row.breach & kGroupHalted);
            for (uint32_t a = 0; a < kAccounts; ++a) {
                const int64_t v = notional_[g][a];
                if (v == 0) continue;
                ++row.accounts;
                row.net = sat_add(row.net, v);
                row.gross = sat_add(row.gross, v < 0 ? (v == INT64_MIN ? INT64_MAX : -v) : v);
                const int64_t abs_v = v < 0 ? (v == INT64_MIN ? INT64_MAX : -v) : v;
                if (abs_v > row.largest) row.largest = abs_v;
            }
            const GroupCaps& c = caps_[g];
            if (c.max_gross > 0 && row.gross > c.max_gross) row.breach |= kGroupGross;
            if (c.max_net > 0) {
                const int64_t abs_net = row.net < 0 ? (row.net == INT64_MIN ? INT64_MAX : -row.net) : row.net;
                if (abs_net > c.max_net) row.breach |= kGroupNet;
            }
            if (c.max_acct > 0 && row.largest > c.max_acct) row.breach |= kGroupAcct;
            rows_[g] = row;
        }
    }

    const GroupRow& row(uint32_t group) const noexcept { return rows_[group < kGroups ? group : 0]; }

    bool blocked(uint32_t group) const noexcept {
        if (group >= kGroups) return true;
        return (rows_[group].breach & static_cast<uint8_t>(kGroupGross | kGroupNet | kGroupAcct | kGroupHalted)) != 0;
    }

    // Remaining room before the cap; INT64_MAX means the cap is off and
    // INT64_MIN means the group is already past it.
    int64_t gross_headroom(uint32_t group) const noexcept {
        if (group >= kGroups) return 0;
        const int64_t cap = caps_[group].max_gross;
        if (cap <= 0) return INT64_MAX;
        const int64_t used = rows_[group].gross;
        return used >= cap ? INT64_MIN : cap - used;
    }

    int64_t net_headroom(uint32_t group) const noexcept {
        if (group >= kGroups) return 0;
        const int64_t cap = caps_[group].max_net;
        if (cap <= 0) return INT64_MAX;
        const int64_t net = rows_[group].net;
        const int64_t used = net < 0 ? (net == INT64_MIN ? INT64_MAX : -net) : net;
        return used >= cap ? INT64_MIN : cap - used;
    }

    int64_t stamp_us() const noexcept { return now_us_; }

private:
    static int64_t sat_add(int64_t a, int64_t b) noexcept {
        if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
        if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
        return a + b;
    }

    // Signed quantity times a positive price, saturated at both ends.
    static int64_t mul_sat(int64_t qty, int64_t px) noexcept {
        if (qty > 0) {
            if (qty > INT64_MAX / px) return INT64_MAX;
            return qty * px;
        }
        if (qty == INT64_MIN) return INT64_MIN;
        const int64_t mag = -qty;
        if (mag > INT64_MAX / px) return INT64_MIN;
        return -(mag * px);
    }

    int64_t notional_[kGroups][kAccounts]{};
    GroupCaps caps_[kGroups]{};
    GroupRow rows_[kGroups]{};
    int64_t now_us_{0};
};

}  // namespace pitlane
