#pragma once

#include "pitlane/numeric.hpp"
#include "pitlane/quote.hpp"
#include "pitlane/sheet.hpp"

#include <cstdint>

namespace pitlane {

// Numbers a caller can study before deciding. The library does not choose a side.
// Spread, mid, and imbalance come from the top of the book. Microprice weights each
// price by the size on the other side. A zero size on both sides uses the mid.

struct BookTop {
    int64_t bid_px{0};
    int64_t ask_px{0};
    int64_t bid_qty{0};
    int64_t ask_qty{0};
    int64_t last_px{0};
};

struct Feat {
    int64_t spread{0};
    int64_t mid{0};
    int64_t micro{0};
    int64_t imbalance{0};
};

inline bool feat_of(const BookTop& top, Feat& out) noexcept {
    if (top.bid_px <= 0 || top.ask_px < top.bid_px || top.bid_qty < 0 || top.ask_qty < 0) return false;
    out.spread = top.ask_px - top.bid_px;
    out.mid = top.bid_px + out.spread / 2;
    out.imbalance = top.bid_qty - top.ask_qty;
    const int64_t den = top.bid_qty + top.ask_qty;
    if (den <= 0) {
        out.micro = out.mid;
        return true;
    }
    const Wide left = mul_wide(static_cast<uint64_t>(top.bid_px), static_cast<uint64_t>(top.ask_qty));
    const Wide right = mul_wide(static_cast<uint64_t>(top.ask_px), static_cast<uint64_t>(top.bid_qty));
    Wide sum = left;
    const uint64_t lo = sum.lo + right.lo;
    sum.hi += right.hi + (lo < sum.lo ? 1ull : 0ull);
    sum.lo = lo;
    uint64_t q = 0;
    if (!div_wide(sum, static_cast<uint64_t>(den), q) || q > static_cast<uint64_t>(INT64_MAX)) return false;
    out.micro = static_cast<int64_t>(q);
    return true;
}

// Later prices the caller fills in. A negative later price is unset.
// The result is a whole number of ticks, truncated toward zero.
// A buy gains when the later price is higher. A sell gains when it is lower.

struct Horizon {
    int64_t entry{0};
    int64_t later[4]{-1, -1, -1, -1};
    uint8_t side{0};
};

inline bool horizon_ticks(const Horizon& horizon, int64_t tick, int64_t out[4]) noexcept {
    if (!out || tick <= 0 || horizon.entry <= 0 || !known_side(horizon.side)) return false;
    bool any = false;
    const bool buy = horizon.side == static_cast<uint8_t>(Side::Buy);
    for (int i = 0; i < 4; ++i) {
        if (horizon.later[i] < 0) {
            out[i] = 0;
            continue;
        }
        const int64_t diff = buy ? horizon.later[i] - horizon.entry : horizon.entry - horizon.later[i];
        out[i] = diff / tick;
        any = true;
    }
    return any;
}

// One name points at one underlying. Several names may share that underlying.
// The caller owns the table. A full table refuses a new name.

class RelBook {
public:
    static constexpr int kCap = 64;

    int size() const noexcept { return n_; }

    bool bind(uint32_t name, uint32_t underlying) noexcept {
        if (name == 0 || underlying == 0) return false;
        for (int i = 0; i < n_; ++i) {
            if (name_[i] != name) continue;
            under_[i] = underlying;
            return true;
        }
        if (n_ >= kCap) return false;
        name_[n_] = name;
        under_[n_] = underlying;
        ++n_;
        return true;
    }

    uint32_t underlying(uint32_t name) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (name_[i] == name) return under_[i];
        }
        return 0;
    }

    int related(uint32_t underlying, uint32_t* out, int cap) const noexcept {
        if (!out || cap <= 0 || underlying == 0) return 0;
        int n = 0;
        for (int i = 0; i < n_ && n < cap; ++i) {
            if (under_[i] != underlying) continue;
            out[n++] = name_[i];
        }
        return n;
    }

private:
    uint32_t name_[kCap]{};
    uint32_t under_[kCap]{};
    int n_{0};
};

// Latest factor values, fixed names and a few slots each. A missing name is added
// until the table is full. A later put replaces that slot and leaves the others.

class FactorBoard {
public:
    static constexpr int kNames = 64;
    static constexpr int kSlots = 4;

    bool put(uint32_t name, int slot, int64_t value) noexcept {
        if (name == 0 || slot < 0 || slot >= kSlots) return false;
        Row* row = find(name);
        if (!row) {
            if (n_ >= kNames) return false;
            row = &row_[n_++];
            row->name = name;
            for (int i = 0; i < kSlots; ++i) row->value[i] = 0;
            for (int i = 0; i < kSlots; ++i) row->on[i] = 0;
        }
        row->value[slot] = value;
        row->on[slot] = 1;
        return true;
    }

    bool get(uint32_t name, int slot, int64_t& value) const noexcept {
        if (slot < 0 || slot >= kSlots) return false;
        const Row* row = find(name);
        if (!row || !row->on[slot]) return false;
        value = row->value[slot];
        return true;
    }

private:
    struct Row {
        uint32_t name{0};
        int64_t value[kSlots]{};
        uint8_t on[kSlots]{};
    };

    Row* find(uint32_t name) noexcept {
        for (int i = 0; i < n_; ++i) {
            if (row_[i].name == name) return &row_[i];
        }
        return nullptr;
    }

    const Row* find(uint32_t name) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (row_[i].name == name) return &row_[i];
        }
        return nullptr;
    }

    Row row_[kNames]{};
    int n_{0};
};

// A cursor over a table the caller already holds. It does not read a file.

template <class T>
struct Replay {
    const T* data{nullptr};
    int n{0};
    int at{0};

    bool take(const T*& out) noexcept {
        if (!data || at < 0 || at >= n) return false;
        out = data + at;
        ++at;
        return true;
    }

    void reset() noexcept { at = 0; }
};

// The caller's decision. Returning false means there is no order.
// The library calls this only after the book and the feature are ready.

struct Idea {
    int64_t px{0};
    int64_t qty{0};
    uint8_t side{0};
};

using Choose = bool (*)(const Feat& feat, const BookTop& top, const int64_t* factor, int nfactor, Idea& idea, void* user);

// One research step. The book is updated first. The feature is filled from the new top.
// A null choice, or a choice that returns false, leaves the sheet unchanged and returns ok.
// A choice that returns an order is checked, when a gate is present, and then staged on the sheet.
// The call does not send the order.

inline Cause study_step(QuoteFold<8>& book, const FoldEdit* edits, int nedit, const int64_t* factor, int nfactor,
                        Choose choose, void* user, GateAsk* gate, Sheet* sheet, uint64_t local_id, int64_t stamp,
                        Feat& feat, BookTop& top) noexcept {
    if (nedit < 0 || (nedit > 0 && !edits)) return Cause::BadPrice;
    if (nfactor < 0 || (nfactor > 0 && !factor)) return Cause::BadQty;
    if (!book.apply(edits, nedit)) return Cause::BadPrice;
    pitlane::FoldLevel bid{};
    pitlane::FoldLevel offer{};
    book.copy(1, &bid, 1);
    book.copy(2, &offer, 1);
    top.bid_px = bid.px;
    top.ask_px = offer.px;
    top.bid_qty = bid.qty;
    top.ask_qty = offer.qty;
    top.last_px = 0;
    if (!feat_of(top, feat)) return Cause::BadPrice;
    if (!choose) return Cause::Ok;
    Idea idea;
    if (!choose(feat, top, factor, nfactor, idea, user)) return Cause::Ok;
    if (idea.px <= 0) return Cause::BadPrice;
    if (idea.qty <= 0 || !known_side(idea.side)) return Cause::BadQty;
    if (sheet && local_id == 0) return Cause::BadSymbol;
    if (gate) {
        gate->px = idea.px;
        gate->qty = idea.qty;
        gate->side = idea.side;
        gate->act = Act::Enter;
        const Cause cause = gate_ask(*gate);
        if (cause != Cause::Ok) return cause;
    }
    if (!sheet) return Cause::Ok;
    if (!sheet_open(*sheet, local_id, idea.side, idea.px, idea.qty)) return Cause::BadQty;
    if (!sheet_apply(*sheet, SheetOp::Stage, 0, stamp, Cause::Ok)) {
        *sheet = Sheet{};
        return Cause::AlreadyFinal;
    }
    return Cause::Ok;
}

}  // namespace pitlane
