#pragma once

#include <cstdint>

namespace pitlane {

// Basket netting: folds a basket order set before any child is sent.
// Buys and sells of the same key cancel, so only the difference reaches the
// venue. The key is chosen by scope: an account and counter, an account, or
// the name alone when the account may net across books. Rows keep the order
// they were first added in, the table is fixed at construction, and nothing
// is allocated.

enum : uint8_t {
    kNetAcctCounter = 1,  // net inside one account and counter
    kNetAcct = 2,         // net across counters of one account
    kNetName = 3          // net across accounts, caller checks the permission
};

struct NetCfg {
    uint8_t scope{kNetAcctCounter};
    int64_t lot{1};  // emitted quantity is rounded down to this step
};

struct NetOrder {
    uint32_t acct{0};
    uint32_t counter{0};
    uint32_t name{0};
    uint8_t side{0};  // 1 buy, 2 sell
    int64_t qty{0};
};

// One net instruction. Account and counter come from the first row of the key.
struct NetLeg {
    uint32_t acct{0};
    uint32_t counter{0};
    uint32_t name{0};
    uint8_t side{0};  // 1 buy, 2 sell
    int64_t qty{0};
};

struct NetReport {
    int64_t gross{0};     // every accepted input quantity
    int64_t net{0};       // every emitted quantity
    int64_t residual{0};  // net quantity dropped by the lot step
    int64_t saved{0};     // gross - net - residual, cancelled by offsetting
    int rows_in{0};
    int rows_out{0};
    int dropped{0};  // keys whose net came to zero
    int invalid{0};
    int overflow{0};
};

template <int Cap>
class NetBook {
public:
    static_assert(Cap >= 2 && Cap <= 256, "cap");

    // A zero lot or an unknown scope is refused and leaves the book closed.
    bool open(const NetCfg& c) noexcept {
        if (c.scope < kNetAcctCounter || c.scope > kNetName || c.lot <= 0) return false;
        cfg_ = c;
        n_ = 0;
        gross_ = 0;
        rows_in_ = 0;
        invalid_ = 0;
        overflow_ = 0;
        open_ = true;
        return true;
    }

    bool is_open() const noexcept { return open_; }
    int rows() const noexcept { return n_; }

    // A row with an unknown side, a zero quantity, or no name or account is
    // refused and counted as invalid. A new key beyond Cap is refused and
    // counted as overflow; a row for an existing key is always accepted.
    bool add(const NetOrder& o) noexcept {
        if (!open_) return false;
        if (o.name == 0 || o.acct == 0 || o.qty <= 0 || (o.side != 1 && o.side != 2)) {
            ++invalid_;
            return false;
        }
        const int64_t delta = o.side == 1 ? o.qty : -o.qty;
        const int i = find(o);
        if (i >= 0) {
            accum(net_[i], delta);
            ++rows_in_;
            gross_ += o.qty;
            return true;
        }
        if (n_ >= Cap) {
            ++overflow_;
            return false;
        }
        row_[n_] = o;
        net_[n_] = delta;
        ++n_;
        ++rows_in_;
        gross_ += o.qty;
        return true;
    }

    // Writes one net leg per key in first-seen order. A key whose net is zero
    // is dropped; a net below one lot contributes to `residual`. Returns the
    // number of legs written, or -1 when the output buffer is too small.
    int legs(NetLeg* out, int cap, NetReport& report) const noexcept {
        report = NetReport{};
        report.gross = gross_;
        report.rows_in = rows_in_;
        report.invalid = invalid_;
        report.overflow = overflow_;
        int need = 0;
        for (int i = 0; i < n_; ++i) {
            if (net_[i] != 0) ++need;
        }
        if (need > cap || (need > 0 && !out)) return -1;
        int n = 0;
        for (int i = 0; i < n_; ++i) {
            int64_t q = net_[i] > 0 ? net_[i] : -net_[i];
            if (q == 0) {
                ++report.dropped;
                continue;
            }
            const int64_t whole = (q / cfg_.lot) * cfg_.lot;
            if (whole != q) report.residual += q - whole;
            if (whole <= 0) continue;
            NetLeg& leg = out[n];
            leg = NetLeg{};
            leg.acct = row_[i].acct;
            leg.counter = row_[i].counter;
            leg.name = row_[i].name;
            leg.side = net_[i] > 0 ? 1 : 2;
            leg.qty = whole;
            report.net += whole;
            ++n;
        }
        report.rows_out = n;
        if (report.gross >= report.net && report.gross - report.net >= report.residual) {
            report.saved = report.gross - report.net - report.residual;
        }
        return n;
    }

private:
    bool same(const NetOrder& a, const NetOrder& b) const noexcept {
        if (a.name != b.name) return false;
        if (cfg_.scope == kNetName) return true;
        if (a.acct != b.acct) return false;
        if (cfg_.scope == kNetAcct) return true;
        return a.counter == b.counter;
    }

    int find(const NetOrder& o) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (same(row_[i], o)) return i;
        }
        return -1;
    }

    static void accum(int64_t& net, int64_t delta) noexcept {
        if (delta > 0 && net > INT64_MAX - delta) {
            net = INT64_MAX;
        } else if (delta < 0 && net < INT64_MIN - delta) {
            net = INT64_MIN;
        } else {
            net += delta;
        }
    }

    NetCfg cfg_{};
    bool open_{false};
    int n_{0};
    NetOrder row_[Cap]{};
    int64_t net_[Cap]{};
    int64_t gross_{0};
    int rows_in_{0};
    int invalid_{0};
    int overflow_{0};
};

}  // namespace pitlane
