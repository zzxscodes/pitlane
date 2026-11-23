#pragma once

#include "pitlane/order/lots.hpp"

#include <cstdint>

namespace pitlane {

// Short inventory rebuilt from a liability tape, plus a special-purpose pool.
// Option free size after combined-lot deduction. Integer only.

enum class CreditPool : uint8_t { Normal = 1, Special = 2 };

struct CreditCash {
    int64_t long_qty{0};
    int64_t short_qty{0};
    int64_t special_short{0}; // dedicated short pool, not mixed into normal
};

inline int64_t credit_from_long(const CreditCash& c) noexcept {
    return c.long_qty > 0 ? c.long_qty : 0;
}

inline int64_t credit_from_short(const CreditCash& c) noexcept {
    const int64_t n = c.short_qty > 0 ? c.short_qty : 0;
    const int64_t s = c.special_short > 0 ? c.special_short : 0;
    if (n > INT64_MAX - s)
        return INT64_MAX;
    return n + s;
}

inline CreditPool map_credit(bool special) noexcept {
    return special ? CreditPool::Special : CreditPool::Normal;
}

// Rebuild short from signed liability deltas (positive increases short debt).
inline bool credit_apply_liability(CreditCash& c, int64_t delta, bool special) noexcept {
    if (delta == 0)
        return true;
    int64_t& dst = special ? c.special_short : c.short_qty;
    if (delta > 0) {
        if (dst > INT64_MAX - delta)
            return false;
        dst += delta;
        return true;
    }
    const int64_t mag = -delta;
    if (dst < mag)
        return false;
    dst -= mag;
    return true;
}

inline bool credit_short_ok(const CreditCash& c, int64_t qty, bool special) noexcept {
    if (qty <= 0)
        return false;
    if (special)
        return c.special_short >= qty;
    return c.short_qty >= qty;
}

inline bool credit_to_margin(const CreditCash& c, MarginBook& m) noexcept {
    m.short_avail = credit_from_short(c);
    return true;
}

// Broker credit terms in whole currency units. A counter asset query fills
// these (source 1); an operator or back office may fill them by hand (source 2)
// while no credit adapter is wired. Source 0 means the console has no data and
// must show zeros rather than invent a quota. `assure_ratio_ppm` is kept
// separate so a counter value is reported verbatim; `credit_assure_ppm`
// derives it when the counter did not send one.
struct CreditTerms {
    int64_t asset_balance{0};
    int64_t total_debt{0};
    int64_t fund_debt{0};
    int64_t stock_debt{0};
    int64_t assure_ratio_ppm{0};
    int64_t finance_quota{0};
    int64_t shortsell_quota{0};
    uint8_t source{0}; // 0 none, 1 counter, 2 manual
};

enum : uint8_t {
    kCreditSourceNone = 0,
    kCreditSourceCounter = 1,
    kCreditSourceManual = 2,
};

// Maintenance ratio in ppm: assets over debt. Zero debt reports 0, meaning
// "no debt" rather than a ratio of zero.
inline int64_t credit_assure_ppm(const CreditTerms& t) noexcept {
    if (t.assure_ratio_ppm > 0)
        return t.assure_ratio_ppm;
    if (t.total_debt <= 0)
        return 0;
    if (t.asset_balance <= 0)
        return 0;
    const int64_t a = t.asset_balance;
    const int64_t d = t.total_debt;
    if (a > (INT64_MAX - 500000) / 1000000)
        return INT64_MAX;
    return (a * 1000000 + d / 2) / d;
}

// Available quota left after the used part of each pool.
inline int64_t credit_finance_left(const CreditTerms& t, int64_t used) noexcept {
    if (t.finance_quota <= 0)
        return 0;
    const int64_t left = t.finance_quota - (used > 0 ? used : 0);
    return left > 0 ? left : 0;
}

inline int64_t credit_shortsell_left(const CreditTerms& t, int64_t used) noexcept {
    if (t.shortsell_quota <= 0)
        return 0;
    const int64_t left = t.shortsell_quota - (used > 0 ? used : 0);
    return left > 0 ? left : 0;
}

template <int Cap>
class CreditBook {
public:
    static_assert(Cap >= 1 && Cap <= 64, "cap");

    void reset() noexcept {
        for (int i = 0; i < n_; ++i) {
            acct_[i] = 0;
            terms_[i] = CreditTerms{};
            cash_[i] = CreditCash{};
        }
        n_ = 0;
    }

    int count() const noexcept { return n_; }

    // Replaces the terms of one account. Negative values are refused so a bad
    // counter reply cannot turn into a negative quota.
    bool set(uint32_t acct, const CreditTerms& t) noexcept {
        if (acct == 0)
            return false;
        if (t.asset_balance < 0 || t.total_debt < 0 || t.fund_debt < 0 || t.stock_debt < 0)
            return false;
        if (t.finance_quota < 0 || t.shortsell_quota < 0 || t.assure_ratio_ppm < 0)
            return false;
        const int i = index(acct);
        if (i >= 0) {
            terms_[i] = t;
            return true;
        }
        if (n_ >= Cap)
            return false;
        acct_[n_] = acct;
        terms_[n_] = t;
        cash_[n_] = CreditCash{};
        ++n_;
        return true;
    }

    // Short inventory for the account, rebuilt from the liability tape.
    bool set_cash(uint32_t acct, const CreditCash& c) noexcept {
        const int i = index(acct);
        if (i < 0) {
            CreditTerms none{};
            if (!set(acct, none))
                return false;
            return set_cash(acct, c);
        }
        cash_[i] = c;
        return true;
    }

    bool get(uint32_t acct, CreditTerms& out) const noexcept {
        const int i = index(acct);
        if (i < 0)
            return false;
        out = terms_[i];
        return true;
    }

    bool get_cash(uint32_t acct, CreditCash& out) const noexcept {
        const int i = index(acct);
        if (i < 0)
            return false;
        out = cash_[i];
        return true;
    }

    bool erase(uint32_t acct) noexcept {
        const int i = index(acct);
        if (i < 0)
            return false;
        for (int j = i; j + 1 < n_; ++j) {
            acct_[j] = acct_[j + 1];
            terms_[j] = terms_[j + 1];
            cash_[j] = cash_[j + 1];
        }
        --n_;
        acct_[n_] = 0;
        terms_[n_] = CreditTerms{};
        cash_[n_] = CreditCash{};
        return true;
    }

    int list(uint32_t* accts, CreditTerms* terms, CreditCash* cash, int cap) const noexcept {
        if (n_ > cap || (n_ > 0 && (!accts || !terms || !cash)))
            return -1;
        for (int i = 0; i < n_; ++i) {
            accts[i] = acct_[i];
            terms[i] = terms_[i];
            cash[i] = cash_[i];
        }
        return n_;
    }

private:
    int index(uint32_t acct) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (acct_[i] == acct)
                return i;
        }
        return -1;
    }

    uint32_t acct_[Cap]{};
    CreditTerms terms_[Cap]{};
    CreditCash cash_[Cap]{};
    int n_{0};
};

// Option free size: long and short nets after combined (combo) lots are reserved.
inline int64_t option_free(int64_t long_qty, int64_t short_qty, int64_t combo) noexcept {
    if (combo < 0)
        combo = 0;
    int64_t L = long_qty > 0 ? long_qty : 0;
    int64_t S = short_qty > 0 ? short_qty : 0;
    if (combo > 0) {
        const int64_t take = L < combo ? L : combo;
        L -= take;
        combo -= take;
    }
    if (combo > 0) {
        const int64_t take = S < combo ? S : combo;
        S -= take;
    }
    if (L > INT64_MAX - S)
        return INT64_MAX;
    return L + S;
}

} // namespace pitlane
