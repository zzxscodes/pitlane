#pragma once

#include "pitlane/desk/broker.hpp"
#include "pitlane/desk/cash.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <cstring>

namespace pitlane {

// Fixed trader → many accounts. Strings are fixed fields; miss/full fail-closed.

template <int Cap, int Per = 8>
class TraderAcctTab {
public:
    static_assert(Cap >= 1 && Cap <= 256, "traders");
    static_assert(Per >= 1 && Per <= 32, "per");

    bool link(const char* trader, uint32_t acct) noexcept {
        if (!trader || trader[0] == 0 || acct == 0) return false;
        int t = find_trader(trader);
        if (t < 0) {
            for (int i = 0; i < Cap; ++i) {
                if (row_[i].used) continue;
                if (!copy_name(row_[i].trader, 32, trader)) return false;
                row_[i].used = 1;
                row_[i].n = 0;
                t = i;
                break;
            }
            if (t < 0) return false;
        }
        for (int i = 0; i < row_[t].n; ++i) {
            if (row_[t].acct[i] == acct) return true;
        }
        if (row_[t].n >= Per) return false;
        row_[t].acct[row_[t].n++] = acct;
        return true;
    }

    int accts(const char* trader, uint32_t* out, int cap) const noexcept {
        if (!trader || !out || cap <= 0) return -1;
        const int t = find_trader(trader);
        if (t < 0) return -1;
        const int n = row_[t].n < cap ? row_[t].n : cap;
        for (int i = 0; i < n; ++i) out[i] = row_[t].acct[i];
        return n;
    }

    bool unlink(const char* trader, uint32_t acct) noexcept {
        const int t = find_trader(trader);
        if (t < 0) return false;
        for (int i = 0; i < row_[t].n; ++i) {
            if (row_[t].acct[i] != acct) continue;
            for (int j = i + 1; j < row_[t].n; ++j) row_[t].acct[j - 1] = row_[t].acct[j];
            --row_[t].n;
            return true;
        }
        return false;
    }

private:
    struct Row {
        char trader[32]{};
        uint32_t acct[Per]{};
        int n{0};
        uint8_t used{0};
    };

    static bool copy_name(char* dst, int cap, const char* src) noexcept {
        int n = 0;
        for (; src[n]; ++n) {
            if (n + 1 >= cap) return false;
        }
        for (int i = 0; i < n; ++i) dst[i] = src[i];
        for (int i = n; i < cap; ++i) dst[i] = 0;
        return true;
    }

    int find_trader(const char* trader) const noexcept {
        for (int i = 0; i < Cap; ++i) {
            if (!row_[i].used) continue;
            if (std::strcmp(row_[i].trader, trader) == 0) return i;
        }
        return -1;
    }

    Row row_[Cap]{};
};

// No live orders, no inventory, no unsettled cash hold. Caller supplies live count for the account.

PITLANE_INLINE bool acct_idle(const CashLine* cash, const HoldLine* hold, int live_orders) noexcept {
    if (live_orders > 0) return false;
    if (cash && cash->held_amt != 0) return false;
    if (hold) {
        if (hold->long_today != 0 || hold->long_yday != 0 || hold->short_today != 0 || hold->short_yday != 0)
            return false;
    }
    return true;
}

}  // namespace pitlane
