#pragma once

#include <cstdint>

namespace pitlane {

struct RankPx {
    int64_t px{0};
    int32_t qty{0};
    void clear() noexcept {
        px = 0;
        qty = 0;
    }
};

// Fixed-depth display book. Ranks hold pointers into a pool of records.
// Insert and erase move pointers, not the price records behind them.
template <int Depth>
class DepthBoard {
public:
    static_assert(Depth > 1, "depth");

    DepthBoard() noexcept {
        for (int i = 0; i < Depth; ++i) {
            bid_[i] = &rec_[i];
            ask_[i] = &rec_[Depth + i];
        }
    }

    void insert_bid(int at, int64_t px, int32_t qty) noexcept { insert(bid_, at, px, qty); }
    void insert_ask(int at, int64_t px, int32_t qty) noexcept { insert(ask_, at, px, qty); }
    void erase_bid(int at) noexcept { erase(bid_, at); }
    void erase_ask(int at) noexcept { erase(ask_, at); }

    void copy_bid(RankPx* dst) const noexcept { copy_out(bid_, dst); }
    void copy_ask(RankPx* dst) const noexcept { copy_out(ask_, dst); }

private:
    using Row = RankPx*;

    static void insert(Row* row, int at, int64_t px, int32_t qty) noexcept {
        if (at < 0 || at >= Depth) return;
        if (at < Depth - 1) {
            RankPx* tail = row[Depth - 1];
            for (int i = Depth - 1; i > at; --i) row[i] = row[i - 1];
            row[at] = tail;
        }
        row[at]->px = px;
        row[at]->qty = qty;
    }

    static void erase(Row* row, int at) noexcept {
        if (at < 0 || at >= Depth) return;
        RankPx* gone = row[at];
        for (int i = at; i < Depth - 1; ++i) row[i] = row[i + 1];
        row[Depth - 1] = gone;
        gone->clear();
    }

    static void copy_out(Row const* row, RankPx* dst) noexcept {
        for (int i = 0; i < Depth; ++i) {
            dst[i].px = row[i]->px;
            dst[i].qty = row[i]->qty;
        }
    }

    RankPx rec_[Depth * 2]{};
    Row bid_[Depth]{};
    Row ask_[Depth]{};
};

// Aggregated quote side. Prices are sorted so the best price sits at the back.
// Existing rungs update in place. Opening a new rung is the cold path.
class AggSide {
public:
    static constexpr int Cap = 64;

    explicit AggSide(bool bid_book) noexcept : bid_(bid_book) {}

    int size() const noexcept { return n_; }

    // q > 0 adds, q < 0 removes. A rung that falls to zero is dropped.
    bool apply(int64_t px, int64_t q) noexcept {
        int i = n_ - 1;
        while (i >= 0 && worse(px_[i], px)) --i;
        if (i >= 0 && px_[i] == px) {
            px_qty_[i] += q;
            if (px_qty_[i] <= 0) {
                for (int j = i; j < n_ - 1; ++j) {
                    px_[j] = px_[j + 1];
                    px_qty_[j] = px_qty_[j + 1];
                }
                --n_;
            }
            return true;
        }
        if (q <= 0) return false;
        if (n_ >= Cap) return false;
        const int at = i + 1;
        for (int j = n_; j > at; --j) {
            px_[j] = px_[j - 1];
            px_qty_[j] = px_qty_[j - 1];
        }
        px_[at] = px;
        px_qty_[at] = q;
        ++n_;
        return true;
    }

    bool top(int64_t* px, int64_t* qty) const noexcept {
        if (n_ <= 0) return false;
        if (px) *px = px_[n_ - 1];
        if (qty) *qty = px_qty_[n_ - 1];
        return true;
    }

private:
    bool worse(int64_t stored, int64_t incoming) const noexcept {
        return bid_ ? stored > incoming : stored < incoming;
    }

    int64_t px_[Cap]{};
    int64_t px_qty_[Cap]{};
    int n_{0};
    bool bid_;
};

}  // namespace pitlane
