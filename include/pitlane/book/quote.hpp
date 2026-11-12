#pragma once

#include "pitlane/util/frame.hpp"

#include <cstdint>
#include <cstring>

namespace pitlane {

// Quote book, separate from the matching book. One batch either lands whole or not at all.
// Prints are recorded beside the book and do not change its levels.

enum class FoldOp : uint8_t { Add = 1, Set = 2, Del = 3 };

struct FoldEdit {
    FoldOp op{FoldOp::Set};
    uint8_t side{0};  // 1 bid, 2 ask
    int64_t px{0};
    int64_t qty{0};
};

struct FoldLevel {
    int64_t px{0};
    int64_t qty{0};
};

template <int Cap>
class QuoteFold {
public:
    static_assert(Cap >= 2 && Cap <= 64, "quote depth");

    bool built() const noexcept { return built_ != 0; }
    int bid_count() const noexcept { return nb_; }
    int ask_count() const noexcept { return na_; }

    // Best price is reported first. Returns the number of levels copied.
    int copy(uint8_t side, FoldLevel* out, int cap) const noexcept {
        if (!out || cap <= 0) return 0;
        const bool bid = side == 1;
        if (side != 1 && side != 2) return 0;
        const int n = bid ? nb_ : na_;
        const int64_t* px = bid ? bpx_ : apx_;
        const int64_t* qty = bid ? bqty_ : aqty_;
        const int take = cap < n ? cap : n;
        for (int i = 0; i < take; ++i) {
            const int at = n - 1 - i;
            out[i].px = px[at];
            out[i].qty = qty[at];
        }
        return take;
    }

    bool apply(const FoldEdit* edits, int n) noexcept {
        if (n < 0 || (n > 0 && !edits)) return false;
        if (n == 0) return true;
        int64_t bpx[Cap];
        int64_t bqty[Cap];
        int64_t apx[Cap];
        int64_t aqty[Cap];
        std::memcpy(bpx, bpx_, sizeof(bpx));
        std::memcpy(bqty, bqty_, sizeof(bqty));
        std::memcpy(apx, apx_, sizeof(apx));
        std::memcpy(aqty, aqty_, sizeof(aqty));
        int nb = nb_;
        int na = na_;
        for (int i = 0; i < n; ++i) {
            if (!one(edits[i], bpx, bqty, nb, apx, aqty, na)) return false;
        }
        std::memcpy(bpx_, bpx, sizeof(bpx));
        std::memcpy(bqty_, bqty, sizeof(bqty));
        std::memcpy(apx_, apx, sizeof(apx));
        std::memcpy(aqty_, aqty, sizeof(aqty));
        nb_ = nb;
        na_ = na;
        built_ = 1;
        return true;
    }

    // Best bid/ask. Fail-closed when a side is empty.
    bool top(int64_t* bid_px, int64_t* bid_qty, int64_t* ask_px, int64_t* ask_qty) const noexcept {
        if (nb_ <= 0 || na_ <= 0) return false;
        if (bid_px) *bid_px = bpx_[nb_ - 1];
        if (bid_qty) *bid_qty = bqty_[nb_ - 1];
        if (ask_px) *ask_px = apx_[na_ - 1];
        if (ask_qty) *ask_qty = aqty_[na_ - 1];
        return true;
    }

    void clear() noexcept {
        nb_ = 0;
        na_ = 0;
        built_ = 0;
        last_seq_ = 0;
        seq_on_ = 0;
    }

    // Contiguous sequence only. Gap or old seq leaves the book and watch unchanged (fail-closed).
    // Source failover stays outside the library.
    bool apply_ordered(uint64_t seq, const FoldEdit* edits, int n, SeqWatch& watch) noexcept {
        if (seq == 0) return false;
        if (!watch.started) {
            if (!apply(edits, n)) return false;
            watch.started = 1;
            watch.next = seq + 1;
            last_seq_ = seq;
            seq_on_ = 1;
            return true;
        }
        if (seq != watch.next) return false;
        if (!apply(edits, n)) return false;
        watch.next = seq + 1;
        last_seq_ = seq;
        seq_on_ = 1;
        return true;
    }

    uint64_t last_seq() const noexcept { return last_seq_; }
    bool seq_on() const noexcept { return seq_on_ != 0; }

private:
    static bool worse(bool bid, int64_t stored, int64_t incoming) noexcept {
        return bid ? stored > incoming : stored < incoming;
    }

    static int find(const int64_t* px, int n, bool bid, int64_t price) noexcept {
        int i = n - 1;
        while (i >= 0 && worse(bid, px[i], price)) --i;
        if (i >= 0 && px[i] == price) return i;
        return -1;
    }

    static bool insert_at(int64_t* px, int64_t* qty, int& n, bool bid, int64_t price, int64_t q) noexcept {
        if (q <= 0) return false;
        if (n >= Cap) return false;
        int i = n - 1;
        while (i >= 0 && worse(bid, px[i], price)) --i;
        if (i >= 0 && px[i] == price) return false;
        const int at = i + 1;
        for (int j = n; j > at; --j) {
            px[j] = px[j - 1];
            qty[j] = qty[j - 1];
        }
        px[at] = price;
        qty[at] = q;
        ++n;
        return true;
    }

    static void erase_at(int64_t* px, int64_t* qty, int& n, int at) noexcept {
        for (int j = at; j < n - 1; ++j) {
            px[j] = px[j + 1];
            qty[j] = qty[j + 1];
        }
        --n;
    }

    static bool one(const FoldEdit& e, int64_t* bpx, int64_t* bqty, int& nb, int64_t* apx, int64_t* aqty, int& na) noexcept {
        if (e.px <= 0) return false;
        if (e.side != 1 && e.side != 2) return false;
        const bool bid = e.side == 1;
        int64_t* px = bid ? bpx : apx;
        int64_t* qty = bid ? bqty : aqty;
        int& n = bid ? nb : na;
        const int at = find(px, n, bid, e.px);
        if (e.op == FoldOp::Add) {
            if (e.qty <= 0 || at >= 0) return false;
            return insert_at(px, qty, n, bid, e.px, e.qty);
        }
        if (e.op == FoldOp::Set) {
            if (e.qty < 0) return false;
            if (e.qty == 0) {
                if (at >= 0) erase_at(px, qty, n, at);
                return true;
            }
            if (at >= 0) {
                qty[at] = e.qty;
                return true;
            }
            return insert_at(px, qty, n, bid, e.px, e.qty);
        }
        if (e.op == FoldOp::Del) {
            if (at >= 0) erase_at(px, qty, n, at);
            return true;
        }
        return false;
    }

    int64_t bpx_[Cap]{};
    int64_t bqty_[Cap]{};
    int64_t apx_[Cap]{};
    int64_t aqty_[Cap]{};
    int nb_{0};
    int na_{0};
    uint8_t built_{0};
    uint64_t last_seq_{0};
    uint8_t seq_on_{0};
};

template <int Cap>
class PrintLog {
public:
    static_assert(Cap >= 1 && Cap <= 256, "prints");

    int size() const noexcept { return n_; }

    bool push(int64_t px, int64_t qty, uint8_t aggressor) noexcept {
        if (n_ >= Cap || px <= 0 || qty <= 0) return false;
        px_[n_] = px;
        qty_[n_] = qty;
        agg_[n_] = aggressor;
        ++n_;
        return true;
    }

    bool at(int i, int64_t* px, int64_t* qty, uint8_t* aggressor) const noexcept {
        if (i < 0 || i >= n_) return false;
        if (px) *px = px_[i];
        if (qty) *qty = qty_[i];
        if (aggressor) *aggressor = agg_[i];
        return true;
    }

private:
    int64_t px_[Cap]{};
    int64_t qty_[Cap]{};
    uint8_t agg_[Cap]{};
    int n_{0};
};

// High, low, and volume over trailing minutes. Time only moves forward.
template <int Cap>
class DayRoll {
public:
    static_assert(Cap >= 2 && Cap <= 512, "minutes");

    int size() const noexcept { return n_; }

    bool on_msg(uint64_t ts_us, int64_t px, int64_t qty) noexcept {
        constexpr uint64_t kMinute = 60000000ull;
        constexpr uint64_t kDay = 86400000000ull;
        if (started_ && ts_us < cursor_) return false;
        cursor_ = ts_us;
        started_ = 1;
        const uint64_t minute = ts_us / kMinute;
        while (n_ > 0) {
            const uint64_t born = bar_[0].minute * kMinute;
            if (born > ts_us || ts_us < born + kDay) break;
            for (int i = 1; i < n_; ++i) bar_[i - 1] = bar_[i];
            --n_;
        }
        if (n_ > 0 && bar_[n_ - 1].minute == minute) {
            if (px > 0) {
                if (bar_[n_ - 1].hi == 0 || px > bar_[n_ - 1].hi) bar_[n_ - 1].hi = px;
                if (bar_[n_ - 1].lo == 0 || px < bar_[n_ - 1].lo) bar_[n_ - 1].lo = px;
            }
            if (qty > 0) bar_[n_ - 1].vol += qty;
            return true;
        }
        if (n_ == Cap) {
            for (int i = 1; i < n_; ++i) bar_[i - 1] = bar_[i];
            --n_;
        }
        Bar bar;
        bar.minute = minute;
        bar.hi = px > 0 ? px : 0;
        bar.lo = px > 0 ? px : 0;
        bar.vol = qty > 0 ? qty : 0;
        bar_[n_++] = bar;
        return true;
    }

    bool at(int i, uint64_t* minute, int64_t* hi, int64_t* lo, int64_t* vol) const noexcept {
        if (i < 0 || i >= n_) return false;
        if (minute) *minute = bar_[i].minute;
        if (hi) *hi = bar_[i].hi;
        if (lo) *lo = bar_[i].lo;
        if (vol) *vol = bar_[i].vol;
        return true;
    }

private:
    struct Bar {
        uint64_t minute{0};
        int64_t hi{0};
        int64_t lo{0};
        int64_t vol{0};
    };
    Bar bar_[Cap]{};
    int n_{0};
    uint64_t cursor_{0};
    uint8_t started_{0};
};

}  // namespace pitlane
