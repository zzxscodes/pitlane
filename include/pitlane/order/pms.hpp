#pragma once

#include "pitlane/desk/types.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Position management. One row per account and name. A fill closes the opposite side up to
// close_qty and opens the rest. Yesterday quantity is consumed first when a close ticks yday.
// Opening always adds today quantity, so the yday fields only change on closes.
// All updates are computed on a copy and committed only when every check passes.

struct PmsFill {
    uint32_t acct{0};
    uint32_t name{0};
    uint8_t side{0};
    int64_t qty{0};
    int64_t px{0};
    int64_t fee{0};
    int64_t close_qty{0};
    int64_t close_yday{0};
};

struct PmsPos {
    uint32_t acct{0};
    uint32_t name{0};
    int64_t long_qty{0};
    int64_t long_yday{0};
    int64_t short_qty{0};
    int64_t short_yday{0};
    int64_t avg_long_px{0};
    int64_t avg_short_px{0};
    int64_t realized{0};
    int64_t fees{0};
};

template <int Cap = 1024>
class Pms {
public:
    static_assert(Cap >= 2 && Cap <= 4096 && (Cap & (Cap - 1)) == 0, "pms capacity");

    PITLANE_HOT bool apply(const PmsFill& f) noexcept {
        if (unlikely(f.qty <= 0 || f.px <= 0 || !known_side(f.side) || f.fee < 0 || f.acct == 0)) return false;
        if (f.close_qty < 0 || f.close_qty > f.qty) return false;
        if (f.close_yday < 0 || f.close_yday > f.close_qty) return false;
        const int idx = find_index(f.acct, f.name);
        PmsPos next;
        if (idx >= 0) {
            next = pos_[idx];
        } else {
            if (n_ >= Cap) return false;
            next.acct = f.acct;
            next.name = f.name;
        }
        if (!merge(next, f)) return false;
        if (idx >= 0) {
            pos_[idx] = next;
        } else {
            pos_[n_] = next;
            ++n_;
        }
        return true;
    }

    const PmsPos* find(uint32_t acct, uint32_t name) const noexcept {
        const int i = find_index(acct, name);
        return i < 0 ? nullptr : &pos_[i];
    }

    // Mark-to-market on both legs. Fails closed when the row is missing or the product overflows.
    bool unrealized(uint32_t acct, uint32_t name, int64_t mark_px, int64_t& out) const noexcept {
        const PmsPos* p = find(acct, name);
        if (unlikely(!p)) return false;
        int64_t total = 0;
        if (p->long_qty > 0) {
            int64_t delta = 0;
            if (!scaled_pnl(p->avg_long_px, mark_px, p->long_qty, true, delta)) return false;
            if (!add_ok(total, delta, total)) return false;
        }
        if (p->short_qty > 0) {
            int64_t delta = 0;
            if (!scaled_pnl(p->avg_short_px, mark_px, p->short_qty, false, delta)) return false;
            if (!add_ok(total, delta, total)) return false;
        }
        out = total;
        return true;
    }

    int64_t net_qty(uint32_t acct, uint32_t name) const noexcept {
        const PmsPos* p = find(acct, name);
        if (!p) return 0;
        return p->long_qty - p->short_qty;
    }

    int size() const noexcept { return n_; }

private:
    static bool add_ok(int64_t a, int64_t b, int64_t& out) noexcept {
        if (b > 0 && a > INT64_MAX - b) return false;
        if (b < 0 && a < INT64_MIN - b) return false;
        out = a + b;
        return true;
    }

    static bool mul_ok(int64_t a, int64_t b, int64_t& out) noexcept {
        if (a < 0 || b < 0) return false;
        if (a == 0 || b == 0) {
            out = 0;
            return true;
        }
        if (a > INT64_MAX / b) return false;
        out = a * b;
        return true;
    }

    // Price difference times quantity. The result is a signed profit.
    static bool scaled_pnl(int64_t entry_px, int64_t px, int64_t qty, bool long_leg, int64_t& out) noexcept {
        const int64_t diff = px >= entry_px ? px - entry_px : entry_px - px;
        int64_t delta = 0;
        if (!mul_ok(diff, qty, delta)) return false;
        const bool gain = long_leg ? (px >= entry_px) : (entry_px >= px);
        out = gain ? delta : -delta;
        return true;
    }

    static bool close_short(PmsPos& p, const PmsFill& f) noexcept {
        int64_t q = f.close_qty;
        if (q > p.short_qty) q = p.short_qty;
        if (q <= 0) return true;
        int64_t yday = f.close_yday;
        if (yday > p.short_yday) yday = p.short_yday;
        if (yday > q) yday = q;
        int64_t delta = 0;
        if (!scaled_pnl(p.avg_short_px, f.px, q, false, delta)) return false;
        if (!add_ok(p.realized, delta, p.realized)) return false;
        p.short_yday -= yday;
        p.short_qty -= q;
        if (p.short_qty == 0) {
            p.short_yday = 0;
            p.avg_short_px = 0;
        }
        return true;
    }

    static bool close_long(PmsPos& p, const PmsFill& f) noexcept {
        int64_t q = f.close_qty;
        if (q > p.long_qty) q = p.long_qty;
        if (q <= 0) return true;
        int64_t yday = f.close_yday;
        if (yday > p.long_yday) yday = p.long_yday;
        if (yday > q) yday = q;
        int64_t delta = 0;
        if (!scaled_pnl(p.avg_long_px, f.px, q, true, delta)) return false;
        if (!add_ok(p.realized, delta, p.realized)) return false;
        p.long_yday -= yday;
        p.long_qty -= q;
        if (p.long_qty == 0) {
            p.long_yday = 0;
            p.avg_long_px = 0;
        }
        return true;
    }

    static bool open_long(PmsPos& p, int64_t qty, int64_t px) noexcept {
        int64_t total = 0;
        if (!add_ok(p.long_qty, qty, total)) return false;
        if (p.long_qty == 0) {
            p.avg_long_px = px;
        } else {
            int64_t prior = 0;
            int64_t add = 0;
            if (!mul_ok(p.avg_long_px, p.long_qty, prior)) return false;
            if (!mul_ok(px, qty, add)) return false;
            if (!add_ok(prior, add, prior)) return false;
            p.avg_long_px = prior / total;
        }
        p.long_qty = total;
        return true;
    }

    static bool open_short(PmsPos& p, int64_t qty, int64_t px) noexcept {
        int64_t total = 0;
        if (!add_ok(p.short_qty, qty, total)) return false;
        if (p.short_qty == 0) {
            p.avg_short_px = px;
        } else {
            int64_t prior = 0;
            int64_t add = 0;
            if (!mul_ok(p.avg_short_px, p.short_qty, prior)) return false;
            if (!mul_ok(px, qty, add)) return false;
            if (!add_ok(prior, add, prior)) return false;
            p.avg_short_px = prior / total;
        }
        p.short_qty = total;
        return true;
    }

    static bool merge(PmsPos& p, const PmsFill& f) noexcept {
        if (f.side == static_cast<uint8_t>(Side::Buy)) {
            if (!close_short(p, f)) return false;
            const int64_t open = f.qty - f.close_qty;
            if (open > 0 && !open_long(p, open, f.px)) return false;
        } else {
            if (!close_long(p, f)) return false;
            const int64_t open = f.qty - f.close_qty;
            if (open > 0 && !open_short(p, open, f.px)) return false;
        }
        return add_ok(p.fees, f.fee, p.fees);
    }

    int find_index(uint32_t acct, uint32_t name) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (pos_[i].acct == acct && pos_[i].name == name) return i;
        }
        return -1;
    }

    PmsPos pos_[Cap]{};
    int n_{0};
};

}  // namespace pitlane
