#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Market-by-order book builder. Fixed storage, no allocation, no floating
// point. Add / modify / cancel / trade are applied by order id; best and depth
// are derived from the live orders. This is the reconstruction side of an MBO
// feed, kept separate from the quote book so a strategy can compare its own
// view against the venue's top of book.

enum class MboAct : uint8_t {
    Add = 1,
    Modify = 2,
    Cancel = 3,
    Trade = 4
};

enum class MboSide : uint8_t {
    Bid = 1,
    Ask = 2
};

struct MboMsg {
    uint64_t id{0};
    int64_t px{0};
    int64_t qty{0};
    uint8_t side{0};
    uint8_t act{0};
    uint16_t pad{0};
};

struct MboLevel {
    int64_t px{0};
    int64_t qty{0};
    uint32_t orders{0};
    uint32_t pad{0};
};

class MboBook {
public:
    static constexpr int kOrders = 4096;
    static constexpr int kBuckets = 8192;

    MboBook() noexcept { clear(); }

    void clear() noexcept {
        for (int i = 0; i < kBuckets; ++i) bucket_[i] = -1;
        for (int i = 0; i < kOrders; ++i) {
            orders_[i] = Order{};
            orders_[i].next = (i + 1 < kOrders) ? i + 1 : -1;
        }
        free_ = 0;
        live_ = 0;
        seq_ = 0;
    }

    PITLANE_HOT bool apply(const MboMsg& msg) noexcept {
        const uint8_t act = msg.act;
        if (act == static_cast<uint8_t>(MboAct::Add)) return add(msg);
        if (act == static_cast<uint8_t>(MboAct::Modify)) return modify(msg);
        if (act == static_cast<uint8_t>(MboAct::Cancel)) return cancel(msg.id);
        if (act == static_cast<uint8_t>(MboAct::Trade)) return trade(msg);
        return false;
    }

    bool has(uint64_t id) const noexcept { return find(id) >= 0; }
    int live() const noexcept { return live_; }
    uint64_t seq() const noexcept { return seq_; }
    int64_t qty_at(uint8_t side, int64_t px) const noexcept {
        if (!valid_side(side) || px <= 0) return 0;
        int64_t sum = 0;
        for (int i = 0; i < kOrders; ++i) {
            const Order& o = orders_[i];
            if (o.used && o.side == side && o.px == px) sum += o.qty;
        }
        return sum;
    }

    bool best(MboLevel& bid, MboLevel& ask) const noexcept {
        bid = MboLevel{};
        ask = MboLevel{};
        const bool b = depth(static_cast<uint8_t>(MboSide::Bid), &bid, 1) == 1;
        const bool a = depth(static_cast<uint8_t>(MboSide::Ask), &ask, 1) == 1;
        return b && a;
    }

    // Writes up to cap levels, best first, for one side. Returns the count.
    int depth(uint8_t side, MboLevel* out, int cap) const noexcept {
        if (!out || cap <= 0 || !valid_side(side)) return 0;
        const bool is_bid = side == static_cast<uint8_t>(MboSide::Bid);
        int64_t last = is_bid ? INT64_MAX : INT64_MIN;
        int n = 0;
        while (n < cap) {
            bool found = false;
            int64_t best_px = 0;
            for (int i = 0; i < kOrders; ++i) {
                const Order& o = orders_[i];
                if (!o.used || o.side != side) continue;
                if (is_bid) {
                    if (o.px >= last) continue;
                    if (!found || o.px > best_px) {
                        best_px = o.px;
                        found = true;
                    }
                } else {
                    if (o.px <= last) continue;
                    if (!found || o.px < best_px) {
                        best_px = o.px;
                        found = true;
                    }
                }
            }
            if (!found) break;
            int64_t sum = 0;
            uint32_t count = 0;
            for (int i = 0; i < kOrders; ++i) {
                const Order& o = orders_[i];
                if (o.used && o.side == side && o.px == best_px) {
                    sum += o.qty;
                    ++count;
                }
            }
            out[n].px = best_px;
            out[n].qty = sum;
            out[n].orders = count;
            out[n].pad = 0;
            last = best_px;
            ++n;
        }
        return n;
    }

    bool invariant() const noexcept {
        int live = 0;
        for (int i = 0; i < kOrders; ++i) {
            const Order& o = orders_[i];
            if (!o.used) continue;
            if (o.qty <= 0 || o.px <= 0 || !valid_side(o.side) || find(o.id) != i) return false;
            ++live;
        }
        return live == live_;
    }

private:
    struct Order {
        uint64_t id{0};
        int64_t px{0};
        int64_t qty{0};
        int32_t next{-1};
        uint8_t side{0};
        uint8_t used{0};
    };

    static bool valid_side(uint8_t side) noexcept {
        return side == static_cast<uint8_t>(MboSide::Bid) || side == static_cast<uint8_t>(MboSide::Ask);
    }

    static uint32_t hash_id(uint64_t x) noexcept {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33;
        return static_cast<uint32_t>(x);
    }

    int find(uint64_t id) const noexcept {
        if (id == 0) return -1;
        int32_t idx = bucket_[hash_id(id) & (kBuckets - 1u)];
        while (idx >= 0) {
            const Order& o = orders_[idx];
            if (o.used && o.id == id) return idx;
            idx = o.next;
        }
        return -1;
    }

    int insert(uint64_t id, int64_t px, int64_t qty, uint8_t side) noexcept {
        if (free_ < 0) return -1;
        const int idx = free_;
        free_ = orders_[idx].next;
        Order& o = orders_[idx];
        o = Order{};
        o.id = id;
        o.px = px;
        o.qty = qty;
        o.side = side;
        o.used = 1;
        const uint32_t b = hash_id(id) & (kBuckets - 1u);
        o.next = bucket_[b];
        bucket_[b] = idx;
        ++live_;
        return idx;
    }

    bool remove(uint64_t id) noexcept {
        if (id == 0) return false;
        const uint32_t b = hash_id(id) & (kBuckets - 1u);
        int32_t prev = -1;
        int32_t cur = bucket_[b];
        while (cur >= 0) {
            Order& o = orders_[cur];
            if (o.used && o.id == id) {
                if (prev < 0) bucket_[b] = o.next;
                else orders_[prev].next = o.next;
                o.used = 0;
                o.next = free_;
                free_ = cur;
                --live_;
                return true;
            }
            prev = cur;
            cur = o.next;
        }
        return false;
    }

    bool add(const MboMsg& msg) noexcept {
        if (msg.id == 0 || msg.px <= 0 || msg.qty <= 0 || !valid_side(msg.side)) return false;
        if (find(msg.id) >= 0) return false;
        if (insert(msg.id, msg.px, msg.qty, msg.side) < 0) return false;
        ++seq_;
        return true;
    }

    bool modify(const MboMsg& msg) noexcept {
        if (msg.id == 0 || msg.qty < 0) return false;
        const int idx = find(msg.id);
        if (idx < 0) return false;
        if (msg.qty == 0) {
            remove(msg.id);
            ++seq_;
            return true;
        }
        if (msg.px <= 0 || !valid_side(msg.side)) return false;
        Order& o = orders_[idx];
        o.px = msg.px;
        o.qty = msg.qty;
        o.side = msg.side;
        ++seq_;
        return true;
    }

    bool cancel(uint64_t id) noexcept {
        if (!remove(id)) return false;
        ++seq_;
        return true;
    }

    bool trade(const MboMsg& msg) noexcept {
        if (msg.id == 0 || msg.qty <= 0) return false;
        const int idx = find(msg.id);
        if (idx < 0) return false;
        Order& o = orders_[idx];
        if (o.qty <= msg.qty) {
            remove(msg.id);
        } else {
            o.qty -= msg.qty;
        }
        ++seq_;
        return true;
    }

    Order orders_[kOrders]{};
    int32_t bucket_[kBuckets]{};
    int32_t free_{-1};
    int live_{0};
    uint64_t seq_{0};
};

} // namespace pitlane
