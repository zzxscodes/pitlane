#pragma once

#include <cstdint>
#include <cstring>

namespace pitlane {

// Manual pair packages: the operator pairs two instruments, keeps the two legs
// together, and tracks their fills as one unit. This is desk bookkeeping, not a
// strategy: the automated spread engine lives in strat/pair.hpp. Quantities are
// in lots of each leg; ratio_ppm is leg B per leg A, used to derive the B target
// and to rebalance the package after a partial fill. Integer only, fixed slots,
// no allocation.

enum : uint8_t {
    kPkgOpen = 1,
    kPkgClosed = 2,     // both legs reached their target
    kPkgCancelled = 3,  // operator abandoned the package
};

inline const char* pair_package_state_name(uint8_t state) noexcept {
    switch (state) {
        case kPkgOpen: return "open";
        case kPkgClosed: return "closed";
        case kPkgCancelled: return "cancelled";
        default: return "unknown";
    }
}

struct PairPackage {
    uint32_t id{0};
    uint8_t state{kPkgOpen};
    uint32_t name_a{0};
    uint32_t name_b{0};
    int64_t ratio_ppm{1000000};
    int64_t qty_a{0};
    int64_t qty_b{0};
    int64_t filled_a{0};
    int64_t filled_b{0};
    int64_t fx_ppm{1000000};
    char note[24]{};
};

struct PairPackageCfg {
    uint32_t name_a{0};
    uint32_t name_b{0};
    int64_t qty_a{0};
    int64_t ratio_ppm{1000000};
    int64_t fx_ppm{1000000};
    char note[24]{};
};

inline void pair_package_note(char* dst, int cap, const char* src) noexcept {
    if (!dst || cap <= 0) return;
    int i = 0;
    if (src) {
        for (; src[i] && i + 1 < cap; ++i) {
            const char c = src[i];
            dst[i] = (c == ' ' || c == '\t' || c == '\n' || c == '\r') ? '_' : c;
        }
    }
    dst[i] = 0;
}

// ceil(qty * ppm / 1e6), with overflow refused.
inline bool pair_package_scale(int64_t qty, int64_t ppm, int64_t& out) noexcept {
    if (qty <= 0 || ppm <= 0) return false;
    if (qty > (INT64_MAX - 999999) / ppm) return false;
    out = (qty * ppm + 999999) / 1000000;
    return out > 0;
}

template <int Cap>
class PairPackageBook {
public:
    static_assert(Cap >= 1 && Cap <= 256, "cap");

    void reset() noexcept {
        for (int i = 0; i < n_; ++i) row_[i] = PairPackage{};
        n_ = 0;
        next_id_ = 1;
    }

    int count() const noexcept { return n_; }

    // Creates an open package and returns its id, or 0 when the config is
    // refused (missing or equal names, non-positive quantity or ratio).
    uint32_t create(const PairPackageCfg& c) noexcept {
        PairPackage p{};
        if (!build(c, p)) return 0;
        if (n_ >= Cap) return 0;
        p.id = next_id_++;
        p.state = kPkgOpen;
        row_[n_++] = p;
        return p.id;
    }

    // Update is allowed while open. Names are fixed. A new A target must not be
    // below what has already filled on either leg.
    bool update(uint32_t id, const PairPackageCfg& c) noexcept {
        const int i = index(id);
        if (i < 0 || row_[i].state != kPkgOpen) return false;
        if (c.qty_a <= 0 || c.ratio_ppm <= 0 || c.fx_ppm <= 0) return false;
        if (c.name_a != 0 && c.name_a != row_[i].name_a) return false;
        if (c.name_b != 0 && c.name_b != row_[i].name_b) return false;
        int64_t qty_b = 0;
        if (!pair_package_scale(c.qty_a, c.ratio_ppm, qty_b)) return false;
        if (qty_b < row_[i].filled_b || c.qty_a < row_[i].filled_a) return false;
        row_[i].ratio_ppm = c.ratio_ppm;
        row_[i].qty_a = c.qty_a;
        row_[i].qty_b = qty_b;
        row_[i].fx_ppm = c.fx_ppm;
        pair_package_note(row_[i].note, sizeof row_[i].note, c.note);
        finish(i);
        return true;
    }

    // Cancelling abandons the package; it does not touch working orders.
    bool cancel(uint32_t id) noexcept {
        const int i = index(id);
        if (i < 0 || row_[i].state != kPkgOpen) return false;
        row_[i].state = kPkgCancelled;
        return true;
    }

    // Closing is an explicit operator action and needs both legs complete.
    bool close(uint32_t id) noexcept {
        const int i = index(id);
        if (i < 0 || row_[i].state != kPkgOpen) return false;
        if (row_[i].filled_a != row_[i].qty_a || row_[i].filled_b != row_[i].qty_b) return false;
        row_[i].state = kPkgClosed;
        return true;
    }

    // Records a fill on leg 1 (A) or 2 (B). A fill beyond the target is
    // refused. The package closes itself once both legs are complete.
    bool fill(uint32_t id, int leg, int64_t qty) noexcept {
        const int i = index(id);
        if (i < 0 || row_[i].state != kPkgOpen || qty <= 0) return false;
        if (leg == 1) {
            if (row_[i].filled_a > row_[i].qty_a - qty) return false;
            row_[i].filled_a += qty;
        } else if (leg == 2) {
            if (row_[i].filled_b > row_[i].qty_b - qty) return false;
            row_[i].filled_b += qty;
        } else {
            return false;
        }
        finish(i);
        return true;
    }

    bool get(uint32_t id, PairPackage& out) const noexcept {
        const int i = index(id);
        if (i < 0) return false;
        out = row_[i];
        return true;
    }

    // Copies packages in creation order. Returns the count, or -1 when the
    // output buffer is too small.
    int list(PairPackage* out, int cap) const noexcept {
        if (n_ > cap || (n_ > 0 && !out)) return -1;
        for (int i = 0; i < n_; ++i) out[i] = row_[i];
        return n_;
    }

    // Restores a package loaded from disk, keeping its id and state.
    bool restore(const PairPackage& p) noexcept {
        if (n_ >= Cap || p.id == 0 || p.name_a == 0 || p.name_b == 0 || p.name_a == p.name_b) return false;
        if (p.qty_a <= 0 || p.qty_b <= 0 || p.ratio_ppm <= 0 || p.fx_ppm <= 0) return false;
        if (p.filled_a < 0 || p.filled_b < 0 || p.filled_a > p.qty_a || p.filled_b > p.qty_b) return false;
        if (p.state < kPkgOpen || p.state > kPkgCancelled) return false;
        if (index(p.id) >= 0) return false;
        row_[n_++] = p;
        if (p.id >= next_id_) next_id_ = p.id + 1;
        return true;
    }

private:
    bool build(const PairPackageCfg& c, PairPackage& p) const noexcept {
        if (c.name_a == 0 || c.name_b == 0 || c.name_a == c.name_b) return false;
        if (c.qty_a <= 0 || c.ratio_ppm <= 0 || c.fx_ppm <= 0) return false;
        int64_t qty_b = 0;
        if (!pair_package_scale(c.qty_a, c.ratio_ppm, qty_b)) return false;
        p.name_a = c.name_a;
        p.name_b = c.name_b;
        p.ratio_ppm = c.ratio_ppm;
        p.qty_a = c.qty_a;
        p.qty_b = qty_b;
        p.fx_ppm = c.fx_ppm;
        pair_package_note(p.note, sizeof p.note, c.note);
        return true;
    }

    void finish(int i) noexcept {
        if (row_[i].filled_a == row_[i].qty_a && row_[i].filled_b == row_[i].qty_b) {
            row_[i].state = kPkgClosed;
        }
    }

    int index(uint32_t id) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (row_[i].id == id) return i;
        }
        return -1;
    }

    PairPackage row_[Cap]{};
    int n_{0};
    uint32_t next_id_{1};
};

}  // namespace pitlane
