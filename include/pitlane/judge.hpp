#pragma once

#include "pitlane/lane.hpp"
#include "pitlane/numeric.hpp"

#include <cstdint>
#include <cstring>

namespace pitlane {

// Pure checks. The caller decides whether a reject is sent, logged, or ignored.

class NameList {
public:
    static constexpr int kCap = 64;
    static constexpr int kLen = 32;

    int size() const noexcept { return n_; }

    bool add(const char* name) noexcept {
        if (!name || name[0] == 0 || has(name) || n_ >= kCap) return false;
        int i = 0;
        for (; i < kLen - 1 && name[i]; ++i) slot_[n_][i] = name[i];
        if (name[i]) return false;
        slot_[n_][i] = 0;
        ++n_;
        return true;
    }

    bool has(const char* name) const noexcept {
        if (!name) return false;
        for (int i = 0; i < n_; ++i) {
            if (std::strcmp(slot_[i], name) == 0) return true;
        }
        return false;
    }

private:
    char slot_[kCap][kLen]{};
    int n_{0};
};

inline Cause list_cause(const NameList& opens, const NameList& closes, bool opening, const char* symbol) noexcept {
    const NameList& list = opening ? opens : closes;
    if (list.has(symbol)) return Cause::Blocked;
    return Cause::Ok;
}

class FlowWindow {
public:
    static constexpr int kCap = 128;

    uint64_t peak() const noexcept { return peak_; }

    int count(int64_t now, int64_t span) const noexcept {
        if (span < 0) return n_;
        const int64_t cut = now - span;
        int n = 0;
        for (int i = 0; i < n_; ++i) {
            const int at = (head_ + i) % kCap;
            if (ts_[at] >= cut) ++n;
        }
        return n;
    }

    bool allow(int64_t now, uint64_t limit, int64_t span) noexcept {
        const uint64_t n = static_cast<uint64_t>(count(now, span)) + 1;
        if (n > peak_) peak_ = n;
        return n < limit;
    }

    bool add(int64_t now, int64_t span) noexcept {
        drop(now, span);
        if (n_ >= kCap) return false;
        const int at = (head_ + n_) % kCap;
        ts_[at] = now;
        ++n_;
        return true;
    }

private:
    void drop(int64_t now, int64_t span) noexcept {
        if (span < 0) return;
        const int64_t cut = now - span;
        while (n_ > 0 && ts_[head_] < cut) {
            head_ = (head_ + 1) % kCap;
            --n_;
        }
    }

    int64_t ts_[kCap]{};
    int head_{0};
    int n_{0};
    uint64_t peak_{0};
};

struct PullTally {
    int64_t placed{0};
    int64_t pulled{0};
    int64_t placed_cash{0};
    int64_t pulled_cash{0};
    int64_t pulls_in_window{0};
};

inline bool count_ratio_ok(const PullTally& tally, int64_t min_n, int64_t cap_num, int64_t cap_den) noexcept {
    if (cap_den <= 0 || cap_num < 0) return false;
    if (tally.placed + 1 < min_n) return true;
    const int64_t num = tally.pulled + 1;
    const int64_t den = tally.placed + 1;
    return cmp_prod(num, cap_den, cap_num, den) <= 0;
}

inline bool cash_ratio_ok(const PullTally& tally, int64_t min_pulls, int64_t cap_num, int64_t cap_den) noexcept {
    if (cap_den <= 0 || cap_num < 0) return false;
    if (tally.placed_cash == 0) return true;
    if (tally.pulls_in_window < min_pulls) return true;
    return cmp_prod(tally.pulled_cash, cap_den, cap_num, tally.placed_cash) < 0;
}

inline Cause pull_ratio_cause(const PullTally& tally, int64_t min_n, int64_t cap_num, int64_t cap_den, int64_t min_pulls,
                              int64_t cash_num, int64_t cash_den) noexcept {
    if (!count_ratio_ok(tally, min_n, cap_num, cap_den)) return Cause::PullRatio;
    if (!cash_ratio_ok(tally, min_pulls, cash_num, cash_den)) return Cause::PullRatio;
    return Cause::Ok;
}

struct DayTally {
    int64_t orders{0};
    int64_t cancels{0};
    int64_t rejects{0};
    int64_t order_cash{0};
    int64_t cancel_cash{0};
    int64_t reject_cash{0};
    int64_t fill_cash{0};
    int64_t buy_fill{0};
    int64_t sell_fill{0};
    int64_t repo_order{0};
    int64_t repo_fill{0};
};

inline bool cancel_ok(const DayTally& day, int64_t cap_num, int64_t cap_den) noexcept {
    if (cap_den <= 0 || cap_num < 0) return false;
    const int64_t den = day.orders - day.cancels - day.rejects;
    if (den <= 0) return true;
    return cmp_prod(day.cancels, cap_den, cap_num, den) < 0;
}

inline bool reject_ok(const DayTally& day, int64_t cap_num, int64_t cap_den) noexcept {
    if (cap_den <= 0 || cap_num < 0) return false;
    const int64_t den = day.orders - day.cancels;
    if (den <= 0) return true;
    return cmp_prod(day.rejects, cap_den, cap_num, den) < 0;
}

inline bool fill_ok(const DayTally& day, int64_t floor_num, int64_t floor_den) noexcept {
    if (floor_den <= 0 || floor_num < 0) return false;
    const int64_t den = (day.order_cash - day.repo_order) - day.cancel_cash - day.reject_cash;
    if (den <= 0) return true;
    const int64_t num = day.fill_cash - day.repo_fill;
    if (num < 0) return false;
    return cmp_prod(num, floor_den, floor_num, den) >= 0;
}

inline int64_t net_buy(const DayTally& day) noexcept { return day.buy_fill - day.sell_fill; }

inline void add_tally(DayTally& dst, const DayTally& src) noexcept {
    dst.orders += src.orders;
    dst.cancels += src.cancels;
    dst.rejects += src.rejects;
    dst.order_cash += src.order_cash;
    dst.cancel_cash += src.cancel_cash;
    dst.reject_cash += src.reject_cash;
    dst.fill_cash += src.fill_cash;
    dst.buy_fill += src.buy_fill;
    dst.sell_fill += src.sell_fill;
    dst.repo_order += src.repo_order;
    dst.repo_fill += src.repo_fill;
}

// Signed price distance from last, scaled by den. Zero when last is not positive.
inline int64_t px_shift(bool buy, int64_t limit, int64_t last, int64_t den) noexcept {
    if (last <= 0 || den <= 0) return 0;
    const int64_t diff = buy ? limit - last : last - limit;
    const int neg = diff < 0;
    const int64_t mag = neg ? -diff : diff;
    const int cmp = cmp_prod(mag, den, INT64_MAX, last);
    if (cmp > 0) return neg ? INT64_MIN + 1 : INT64_MAX;
    Wide w = mul_wide(static_cast<uint64_t>(mag), static_cast<uint64_t>(den));
    uint64_t q = 0;
    if (!div_wide(w, static_cast<uint64_t>(last), q) || q > static_cast<uint64_t>(INT64_MAX)) {
        return neg ? INT64_MIN + 1 : INT64_MAX;
    }
    const int64_t out = static_cast<int64_t>(q);
    return neg ? -out : out;
}

inline Cause deviation_cause(bool buy, int64_t limit, int64_t last, int64_t den, int64_t cap) noexcept {
    if (cap < 0) return Cause::BadPrice;
    const int64_t shift = px_shift(buy, limit, last, den);
    const int64_t mag = shift < 0 ? -shift : shift;
    if (mag > cap) return Cause::BadPrice;
    return Cause::Ok;
}

struct WashRow {
    uint32_t acct{0};
    uint32_t group{0};
    uint32_t name{0};
    int64_t px{0};
    uint8_t side{0};
    uint8_t live{0};
};

inline bool wash_hit(const WashRow* rows, int n, uint32_t acct, uint32_t group, uint32_t name, uint8_t side,
                     int64_t px) noexcept {
    if (!rows || n <= 0 || (side != 1 && side != 2) || px <= 0) return false;
    for (int i = 0; i < n; ++i) {
        const WashRow& row = rows[i];
        if (!row.live || row.name != name || row.side == side || row.px <= 0) continue;
        if (row.side != 1 && row.side != 2) continue;
        const bool same_acct = row.acct == acct;
        const bool same_group = group != 0 && row.group == group;
        if (!same_acct && !same_group) continue;
        if (side == 1 && px >= row.px && row.side == 2) return true;
        if (side == 2 && px <= row.px && row.side == 1) return true;
    }
    return false;
}

inline Cause wash_cause(const WashRow* rows, int n, uint32_t acct, uint32_t group, uint32_t name, uint8_t side,
                        int64_t px) noexcept {
    return wash_hit(rows, n, acct, group, name, side, px) ? Cause::WashRisk : Cause::Ok;
}

}  // namespace pitlane
