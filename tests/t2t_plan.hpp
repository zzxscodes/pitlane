#pragma once

#include <cstdint>

// Modules that belong to the test, not to the library.
// A feature is a number taken from the book. The plan decides whether to send.
// The library does not make that choice.

struct T2TFeature {
    int64_t spread{0};
    int64_t mid{0};
    int64_t imbalance{0};
};

inline bool t2t_feature(int64_t bid, int64_t ask, int64_t bid_qty, int64_t ask_qty, T2TFeature& out) noexcept {
    if (bid <= 0 || ask < bid || bid_qty < 0 || ask_qty < 0) return false;
    out.spread = ask - bid;
    out.mid = bid + (ask - bid) / 2;
    out.imbalance = bid_qty - ask_qty;
    return true;
}

struct T2TPlan {
    int64_t buy_px{0};
    int64_t qty{0};
    int64_t pos{0};
    int64_t pos_limit{0};
    int64_t max_spread{0};
};

struct T2TOrder {
    int64_t px{0};
    int64_t qty{0};
    uint8_t side{0};
};

inline bool t2t_plan_on_book(T2TPlan& plan, const T2TFeature& feature, int64_t bid, int64_t ask, T2TOrder& out) noexcept {
    out.px = 0;
    out.qty = 0;
    out.side = 0;
    if (plan.qty <= 0 || plan.buy_px <= 0 || plan.max_spread <= 0) return false;
    if (bid <= 0 || ask <= 0 || ask < bid) return false;
    if (feature.spread != ask - bid || feature.spread <= 0 || feature.spread > plan.max_spread) return false;
    if (ask > plan.buy_px) return false;
    if (plan.pos > plan.pos_limit - plan.qty) return false;
    out.px = ask;
    out.qty = plan.qty;
    out.side = 1;
    plan.pos += plan.qty;
    return true;
}
