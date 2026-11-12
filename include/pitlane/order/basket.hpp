#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Per-name target ledger for a basket. Control headers stay in basket_make.

struct BasketLeg {
    uint32_t name{0};
    int64_t target{0};
    int64_t done{0};
    int64_t working{0};
};

PITLANE_INLINE int64_t basket_gap(const BasketLeg& leg) noexcept {
    const int64_t used = leg.done + leg.working;
    if (leg.target <= used) return 0;
    return leg.target - used;
}

// Round the remaining gap down to a whole lot. lot <= 0 fails closed (0).
PITLANE_INLINE int64_t basket_topup(const BasketLeg& leg, int64_t lot) noexcept {
    if (lot <= 0) return 0;
    const int64_t gap = basket_gap(leg);
    return (gap / lot) * lot;
}

template <int Cap>
class BasketBook {
public:
    static_assert(Cap >= 1 && Cap <= 256, "cap");

    uint64_t id() const noexcept { return id_; }
    int legs() const noexcept { return n_; }

    bool open(uint64_t id) noexcept {
        if (id == 0) return false;
        id_ = id;
        n_ = 0;
        for (int i = 0; i < Cap; ++i) leg_[i] = BasketLeg{};
        return true;
    }

    int find(uint32_t name) const noexcept {
        if (name == 0) return -1;
        for (int i = 0; i < n_; ++i) {
            if (leg_[i].name == name) return i;
        }
        return -1;
    }

    bool put(uint32_t name, int64_t target) noexcept {
        if (id_ == 0 || name == 0 || target < 0) return false;
        const int i = find(name);
        if (i >= 0) {
            leg_[i].target = target;
            return true;
        }
        if (n_ >= Cap) return false;
        leg_[n_] = BasketLeg{};
        leg_[n_].name = name;
        leg_[n_].target = target;
        ++n_;
        return true;
    }

    BasketLeg* at(int i) noexcept {
        if (i < 0 || i >= n_) return nullptr;
        return &leg_[i];
    }

    const BasketLeg* at(int i) const noexcept {
        if (i < 0 || i >= n_) return nullptr;
        return &leg_[i];
    }

    bool add_done(uint32_t name, int64_t qty) noexcept {
        BasketLeg* leg = mut(name);
        if (!leg || qty <= 0) return false;
        if (leg->working < qty) return false;
        leg->working -= qty;
        leg->done += qty;
        return true;
    }

    bool add_working(uint32_t name, int64_t qty) noexcept {
        BasketLeg* leg = mut(name);
        if (!leg || qty <= 0) return false;
        if (basket_gap(*leg) < qty) return false;
        leg->working += qty;
        return true;
    }

    bool drop_working(uint32_t name, int64_t qty) noexcept {
        BasketLeg* leg = mut(name);
        if (!leg || qty <= 0 || leg->working < qty) return false;
        leg->working -= qty;
        return true;
    }

private:
    BasketLeg* mut(uint32_t name) noexcept {
        const int i = find(name);
        return i < 0 ? nullptr : &leg_[i];
    }

    uint64_t id_{0};
    int n_{0};
    BasketLeg leg_[Cap]{};
};

}  // namespace pitlane
