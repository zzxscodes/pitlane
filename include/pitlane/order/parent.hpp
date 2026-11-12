#pragma once

#include <cstdint>

namespace pitlane {

// Splits a parent quantity across a time grid. The caller sends the child orders.
// A child that does not finish returns its remainder. A fill that arrives after that return
// is taken back from the pool, and any excess is removed from quantity already given to a later child.

struct SlicePlan {
    int64_t total{0};
    int64_t pool{0};
    int64_t scheduled{0};
    int64_t filled{0};
    int64_t per{0};
    int64_t rem{0};
    int64_t interval_us{0};
    int64_t t0{0};
    int cuts{0};
    int sent{0};
    int64_t lot{1};
};

struct SliceCut {
    int64_t qty{0};
    int64_t due_us{0};
    uint8_t last{0};
};

inline bool slice_open(SlicePlan& plan, int64_t total, int64_t horizon_us, int64_t interval_us, int64_t lot,
                       int64_t t0) noexcept {
    plan = SlicePlan{};
    if (total <= 0 || horizon_us <= 0 || interval_us <= 0 || horizon_us < interval_us || lot <= 0) return false;
    if (horizon_us / interval_us > 1000000) return false;
    int cuts = static_cast<int>(horizon_us / interval_us);
    if (cuts <= 0) return false;
    if (total / cuts < lot) {
        if (total < lot) return false;
        cuts = 1;
    }
    const int64_t per = total / cuts;
    plan.total = total;
    plan.pool = total;
    plan.per = per;
    plan.rem = total - per * cuts;
    plan.interval_us = interval_us;
    plan.t0 = t0;
    plan.cuts = cuts;
    plan.lot = lot;
    return true;
}

inline bool slice_poll(SlicePlan& plan, int64_t now_us, SliceCut& cut) noexcept {
    cut = SliceCut{};
    if (plan.sent >= plan.cuts || plan.pool <= 0) return false;
    if (plan.sent > 0 && plan.interval_us > INT64_MAX / plan.sent) return false;
    const int64_t due = plan.t0 + plan.sent * plan.interval_us;
    if (now_us < due) return false;
    int64_t qty = plan.per;
    if (plan.sent + 1 == plan.cuts) {
        if (qty > INT64_MAX - plan.rem) return false;
        qty += plan.rem;
    }
    if (qty > plan.pool) qty = plan.pool;
    if (qty <= 0) return false;
    plan.pool -= qty;
    plan.scheduled += qty;
    ++plan.sent;
    cut.qty = qty;
    cut.due_us = due;
    cut.last = plan.sent == plan.cuts ? 1 : 0;
    return true;
}

inline bool slice_fill(SlicePlan& plan, int64_t qty) noexcept {
    if (qty <= 0 || qty > plan.scheduled) return false;
    plan.scheduled -= qty;
    plan.filled += qty;
    return true;
}

inline bool slice_restore(SlicePlan& plan, int64_t qty) noexcept {
    if (qty <= 0 || qty > plan.scheduled) return false;
    plan.scheduled -= qty;
    plan.pool += qty;
    return true;
}

// Returns the quantity removed from children that were already scheduled again.
inline int64_t slice_late(SlicePlan& plan, int64_t qty) noexcept {
    if (qty <= 0) return 0;
    plan.filled += qty;
    if (plan.pool >= qty) {
        plan.pool -= qty;
        return 0;
    }
    const int64_t over = qty - plan.pool;
    plan.pool = 0;
    const int64_t take = plan.scheduled < over ? plan.scheduled : over;
    plan.scheduled -= take;
    return take;
}

// Minute-weight volume curve (notes §47). Weights are relative; lag is shared over the remaining curve.
template <int N>
struct CurvePlan {
    static_assert(N >= 1 && N <= 64, "curve minutes");
    int64_t total{0};
    int64_t done{0};
    int64_t weight[N]{};
    int64_t weight_sum{0};
    int minutes{0};
    int64_t lot{1};
};

template <int N>
inline bool curve_open(CurvePlan<N>& plan, int64_t total, const int64_t* weights, int minutes, int64_t lot) noexcept {
    plan = CurvePlan<N>{};
    if (total <= 0 || !weights || minutes < 1 || minutes > N || lot <= 0) return false;
    int64_t sum = 0;
    for (int i = 0; i < minutes; ++i) {
        if (weights[i] < 0) return false;
        if (sum > INT64_MAX - weights[i]) return false;
        sum += weights[i];
        plan.weight[i] = weights[i];
    }
    if (sum <= 0) return false;
    plan.total = total;
    plan.weight_sum = sum;
    plan.minutes = minutes;
    plan.lot = lot;
    return true;
}

// Target cumulative quantity through minute index (0-based, inclusive).
template <int N>
inline int64_t curve_target(const CurvePlan<N>& plan, int minute) noexcept {
    if (minute < 0 || plan.minutes <= 0 || plan.weight_sum <= 0) return 0;
    if (minute >= plan.minutes) minute = plan.minutes - 1;
    int64_t w = 0;
    for (int i = 0; i <= minute; ++i) w += plan.weight[i];
    return plan.total * w / plan.weight_sum;
}

// Quantity to send now so lag is shared across the remaining curve from minute onward.
template <int N>
inline int64_t lag_share_remaining(const CurvePlan<N>& plan, int minute) noexcept {
    if (minute < 0) minute = 0;
    if (minute >= plan.minutes) return 0;
    const int64_t target = curve_target(plan, minute);
    int64_t lag = target - plan.done;
    if (lag <= 0) return 0;
    int64_t rest_w = 0;
    for (int i = minute; i < plan.minutes; ++i) rest_w += plan.weight[i];
    if (rest_w <= 0) return 0;
    int64_t share = lag * plan.weight[minute] / rest_w;
    if (share < plan.lot && lag >= plan.lot) share = plan.lot;
    if (share > lag) share = lag;
    if (plan.done > plan.total) return 0;
    const int64_t left = plan.total - plan.done;
    if (share > left) share = left;
    return share;
}

template <int N>
inline bool curve_fill(CurvePlan<N>& plan, int64_t qty) noexcept {
    if (qty <= 0 || plan.done > plan.total - qty) return false;
    plan.done += qty;
    return true;
}

enum class BasketOp : uint8_t { Create = 1, Pause = 2, Resume = 3, Stop = 4, Update = 5 };

struct BasketNote {
    uint64_t id{0};
    BasketOp op{static_cast<BasketOp>(0)};
    char kind[16]{};
    int32_t bytes{0};
};

inline bool basket_make(uint64_t id, BasketOp op, const char* kind, int bytes, int max_bytes, BasketNote& out) noexcept {
    out = BasketNote{};
    if (id == 0 || !kind || kind[0] == 0 || bytes < 0 || max_bytes < 0 || bytes > max_bytes) return false;
    if (op != BasketOp::Create && op != BasketOp::Pause && op != BasketOp::Resume && op != BasketOp::Stop &&
        op != BasketOp::Update) {
        return false;
    }
    int n = 0;
    for (; n < 15 && kind[n]; ++n) out.kind[n] = kind[n];
    if (kind[n]) {
        out = BasketNote{};
        return false;
    }
    out.kind[n] = 0;
    out.id = id;
    out.op = op;
    out.bytes = bytes;
    return true;
}

}  // namespace pitlane
