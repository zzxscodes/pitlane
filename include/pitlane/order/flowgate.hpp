#pragma once

// Rolling order-flow counters for the order path. One ring of one-second
// buckets per account and per market holds order count, cancel count, placed
// notional, and cancelled notional. Lifetime totals back the cancellation
// ratio. Nothing allocates, nothing locks, and a query costs at most
// kBuckets comparisons, so the gate stays usable on the send path.
//
// The trading thread owns writes; a statistics reader may call the query
// methods while the writer runs, because a bucket is rewritten only after its
// epoch no longer matches and stale buckets are ignored by every query.

#include <cstdint>

namespace pitlane {

struct FlowCell {
    int64_t sec{-1};
    int64_t orders{0};
    int64_t cancels{0};
    int64_t notional{0};
    int64_t canceled{0};
};

class FlowGate {
public:
    static constexpr uint32_t kBuckets = 16;
    static constexpr uint32_t kAccounts = 64;
    static constexpr uint32_t kMarkets = 4;

    void reset() noexcept {
        for (uint32_t a = 0; a < kAccounts; ++a) {
            lifetime_orders_[a] = 0;
            lifetime_cancels_[a] = 0;
            lifetime_canceled_[a] = 0;
            for (uint32_t b = 0; b < kBuckets; ++b) acct_[a][b] = FlowCell{};
        }
        for (uint32_t m = 0; m < kMarkets; ++m) {
            for (uint32_t b = 0; b < kBuckets; ++b) market_[m][b] = FlowCell{};
        }
    }

    // Records one order that passed the gate. `notional` is price times
    // quantity in the caller's integer units.
    void on_order(uint32_t acct, uint8_t market, int64_t notional, int64_t now_us) noexcept {
        if (acct >= kAccounts) return;
        FlowCell& c = touch(acct, now_us);
        ++c.orders;
        c.notional += notional;
        ++lifetime_orders_[acct];
        if (market < kMarkets) ++touch_market(market, now_us).orders;
    }

    // Records one cancel that reached the counter. `notional` is the value of
    // the cancelled residual at the order price.
    void on_cancel(uint32_t acct, uint8_t market, int64_t notional, int64_t now_us) noexcept {
        if (acct >= kAccounts) return;
        FlowCell& c = touch(acct, now_us);
        ++c.cancels;
        c.canceled += notional;
        ++lifetime_cancels_[acct];
        lifetime_canceled_[acct] += notional;
        if (market < kMarkets) ++touch_market(market, now_us).cancels;
    }

    int64_t orders(uint32_t acct, int32_t secs, int64_t now_us) const noexcept {
        if (acct >= kAccounts) return 0;
        return sum(acct, secs, now_us, 0);
    }

    int64_t cancels(uint32_t acct, int32_t secs, int64_t now_us) const noexcept {
        if (acct >= kAccounts) return 0;
        return sum(acct, secs, now_us, 1);
    }

    int64_t notional(uint32_t acct, int32_t secs, int64_t now_us) const noexcept {
        if (acct >= kAccounts) return 0;
        return sum(acct, secs, now_us, 2);
    }

    int64_t canceled(uint32_t acct, int32_t secs, int64_t now_us) const noexcept {
        if (acct >= kAccounts) return 0;
        return sum(acct, secs, now_us, 3);
    }

    int64_t market_orders(uint8_t market, int32_t secs, int64_t now_us) const noexcept {
        if (market >= kMarkets) return 0;
        return sum_market(market, secs, now_us, 0);
    }

    int64_t market_cancels(uint8_t market, int32_t secs, int64_t now_us) const noexcept {
        if (market >= kMarkets) return 0;
        return sum_market(market, secs, now_us, 1);
    }

    int64_t lifetime_orders(uint32_t acct) const noexcept {
        return acct < kAccounts ? lifetime_orders_[acct] : 0;
    }

    int64_t lifetime_cancels(uint32_t acct) const noexcept {
        return acct < kAccounts ? lifetime_cancels_[acct] : 0;
    }

    int64_t lifetime_canceled(uint32_t acct) const noexcept {
        return acct < kAccounts ? lifetime_canceled_[acct] : 0;
    }

    // Cancels per million orders, with the incoming order counted in the
    // denominator. Returns 0 until at least one order exists.
    int64_t cancel_ratio_ppm(uint32_t acct) const noexcept {
        if (acct >= kAccounts) return 0;
        const int64_t placed = lifetime_orders_[acct];
        if (placed <= 0) return 0;
        return lifetime_cancels_[acct] * 1000000 / placed;
    }

private:
    static FlowCell empty() noexcept { return FlowCell{}; }

    FlowCell& touch(uint32_t acct, int64_t now_us) noexcept {
        const int64_t sec = now_us / 1000000;
        FlowCell& c = acct_[acct][static_cast<uint32_t>(sec) % kBuckets];
        if (c.sec != sec) {
            c = FlowCell{};
            c.sec = sec;
        }
        return c;
    }

    FlowCell& touch_market(uint8_t market, int64_t now_us) noexcept {
        const int64_t sec = now_us / 1000000;
        FlowCell& c = market_[market][static_cast<uint32_t>(sec) % kBuckets];
        if (c.sec != sec) {
            c = FlowCell{};
            c.sec = sec;
        }
        return c;
    }

    int64_t sum(uint32_t acct, int32_t secs, int64_t now_us, int field) const noexcept {
        if (secs <= 0) return 0;
        if (secs > static_cast<int32_t>(kBuckets)) secs = static_cast<int32_t>(kBuckets);
        const int64_t sec = now_us / 1000000;
        const int64_t from = sec - secs + 1;
        int64_t total = 0;
        for (uint32_t b = 0; b < kBuckets; ++b) {
            const FlowCell& c = acct_[acct][b];
            if (c.sec < from || c.sec > sec) continue;
            total += pick(c, field);
        }
        return total;
    }

    int64_t sum_market(uint8_t market, int32_t secs, int64_t now_us, int field) const noexcept {
        if (secs <= 0) return 0;
        if (secs > static_cast<int32_t>(kBuckets)) secs = static_cast<int32_t>(kBuckets);
        const int64_t sec = now_us / 1000000;
        const int64_t from = sec - secs + 1;
        int64_t total = 0;
        for (uint32_t b = 0; b < kBuckets; ++b) {
            const FlowCell& c = market_[market][b];
            if (c.sec < from || c.sec > sec) continue;
            total += pick(c, field);
        }
        return total;
    }

    static int64_t pick(const FlowCell& c, int field) noexcept {
        switch (field) {
            case 0: return c.orders;
            case 1: return c.cancels;
            case 2: return c.notional;
            default: return c.canceled;
        }
    }

    FlowCell acct_[kAccounts][kBuckets]{};
    FlowCell market_[kMarkets][kBuckets]{};
    int64_t lifetime_orders_[kAccounts]{};
    int64_t lifetime_cancels_[kAccounts]{};
    int64_t lifetime_canceled_[kAccounts]{};
};

}  // namespace pitlane
