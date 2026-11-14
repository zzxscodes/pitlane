#pragma once

// Manual execution: direct orders, target-position baskets, and TWAP algos.
// Every path builds an Intent and hands it to the same send callback the
// strategy uses, so OMS, PMS, and the id mint stay one ledger.

#include "pitlane/desk/ids.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/ops/command.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <ctime>

namespace pitlane {

struct ManualQuote {
    int64_t bid{0};
    int64_t ask{0};
};

using ManualTopFn = void (*)(void* user, uint32_t name, ManualQuote& out);

class ManualBook {
public:
    static constexpr int kBaskets = 2;
    static constexpr int kAlgos = 4;

    // Returns 0 accepted, 1 risk rejected, 2 transport or capacity failure.
    using SendFn = int (*)(void* user, const Intent& in);
    // Signed net position of one name from the shared PMS.
    using NetFn = int64_t (*)(void* user, uint32_t name);

    void open(IdMint* mint, SendFn send, void* user, ManualTopFn top, void* top_user, NetFn net = nullptr,
              void* net_user = nullptr) noexcept {
        mint_ = mint;
        send_ = send;
        send_user_ = user;
        top_ = top;
        top_user_ = top_user;
        net_ = net;
        net_user_ = net_user;
        for (int i = 0; i < kBaskets; ++i) basket_[i] = Basket{};
        for (int i = 0; i < kAlgos; ++i) algo_[i] = Algo{};
    }

    void set_account(uint32_t acct) noexcept { acct_ = acct; }

    int direct(uint32_t acct, uint32_t name, uint8_t side, int64_t qty, int64_t px, int64_t now_us,
               uint64_t* tag_out = nullptr) noexcept {
        if (!known_side(side) || qty <= 0 || name == 0) return kOpsBadReq;
        if (px <= 0) {
            ManualQuote q{};
            if (top_) top_(top_user_, name, q);
            px = side == static_cast<uint8_t>(Side::Buy) ? q.ask : q.bid;
        }
        if (px <= 0) return kOpsBadReq;
        const uint64_t tag = next_tag(now_us);
        if (tag == 0) return kOpsBusy;
        Intent in{};
        in.tag = tag;
        in.name = name;
        in.acct = acct;
        in.px = px;
        in.qty = qty;
        in.stamp_us = now_us;
        in.side = side;
        in.act = static_cast<uint8_t>(Act::Enter);
        in.style = static_cast<uint8_t>(Style::Immediate);
        const int rc = send_ ? send_(send_user_, in) : 2;
        if (rc == 0 && tag_out) *tag_out = tag;
        return rc == 0 ? kOpsOk : (rc == 1 ? kOpsRisk : kOpsBusy);
    }

    // Target positions. A negative target is a short or a full close.
    int basket_open(uint64_t id, const OpsLeg* legs, int n, int64_t now_us) noexcept {
        if (id == 0 || !legs || n <= 0 || n > kOpsBasketLegs) return kOpsBadReq;
        int slot = -1;
        for (int i = 0; i < kBaskets; ++i) {
            if (!basket_[i].active) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return kOpsBusy;
        for (int i = 0; i < n; ++i) {
            if (legs[i].name == 0) return kOpsBadReq;
            for (int j = 0; j < i; ++j) {
                if (legs[j].name == legs[i].name) return kOpsBadReq;
            }
        }
        Basket& b = basket_[slot];
        b = Basket{};
        b.id = id;
        b.n = static_cast<uint8_t>(n);
        for (int i = 0; i < n; ++i) {
            b.leg[i].name = legs[i].name;
            b.leg[i].target = legs[i].target;
            b.leg[i].last_us = now_us - 200000;
        }
        b.active = 1;
        return kOpsOk;
    }

    int algo_open(uint64_t id, uint32_t name, uint8_t side, int64_t qty, int64_t horizon_us, int64_t slice_us,
                  int64_t now_us) noexcept {
        if (id == 0 || name == 0 || !known_side(side) || qty <= 0) return kOpsBadReq;
        if (horizon_us <= 0 || slice_us <= 0 || slice_us > horizon_us) return kOpsBadReq;
        int slot = -1;
        for (int i = 0; i < kAlgos; ++i) {
            if (!algo_[i].active) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return kOpsBusy;
        Algo& a = algo_[slot];
        a = Algo{};
        a.id = id;
        a.name = name;
        a.side = side;
        a.qty = qty;
        a.horizon_us = horizon_us;
        a.slice_us = slice_us;
        a.start_us = now_us;
        a.next_us = now_us;
        a.active = 1;
        return kOpsOk;
    }

    // Sends due basket top-ups and algo slices. Returns the number of children accepted.
    int on_beat(int64_t now_us, int64_t max_qty, int64_t max_notional) noexcept {
        int sent = 0;
        for (int i = 0; i < kBaskets; ++i) {
            if (basket_[i].active && beat_basket(basket_[i], now_us, max_qty, max_notional) > 0) ++sent;
        }
        for (int j = 0; j < kAlgos; ++j) {
            if (algo_[j].active && beat_algo(algo_[j], now_us, max_qty, max_notional) > 0) ++sent;
        }
        return sent;
    }

    void cancel_all() noexcept {
        for (int i = 0; i < kBaskets; ++i) basket_[i].active = 0;
        for (int i = 0; i < kAlgos; ++i) algo_[i].active = 0;
    }

    int baskets_active() const noexcept {
        int n = 0;
        for (int i = 0; i < kBaskets; ++i) n += basket_[i].active ? 1 : 0;
        return n;
    }

    int algos_active() const noexcept {
        int n = 0;
        for (int i = 0; i < kAlgos; ++i) n += algo_[i].active ? 1 : 0;
        return n;
    }

private:
    struct BasketLeg {
        uint32_t name{0};
        int64_t target{0};
        int64_t last_us{0};
    };

    struct Basket {
        uint64_t id{0};
        uint8_t active{0};
        uint8_t n{0};
        BasketLeg leg[kOpsBasketLegs]{};
    };

    struct Algo {
        uint64_t id{0};
        uint32_t name{0};
        uint8_t side{0};
        uint8_t active{0};
        int64_t qty{0};
        int64_t sent{0};
        int64_t horizon_us{0};
        int64_t slice_us{0};
        int64_t start_us{0};
        int64_t next_us{0};
    };

    static int day_index(int64_t now_us) noexcept {
        const time_t sec = static_cast<time_t>(now_us / 1000000);
        std::tm tm{};
        localtime_r(&sec, &tm);
        return tm.tm_yday;
    }

    uint64_t next_tag(int64_t now_us) noexcept {
        if (!mint_) return 0;
        const int64_t sec = now_us / 1000000;
        return mint_->next(day_index(now_us), static_cast<int>(sec % 86400));
    }

    int send_child(uint32_t name, uint8_t side, int64_t qty, int64_t now_us, int64_t max_qty,
                   int64_t max_notional) noexcept {
        if (qty <= 0) return kOpsBadReq;
        if (max_qty > 0 && qty > max_qty) qty = max_qty;
        ManualQuote q{};
        if (top_) top_(top_user_, name, q);
        const int64_t px = side == static_cast<uint8_t>(Side::Buy) ? q.ask : q.bid;
        if (px <= 0) return kOpsBadReq;
        if (max_notional > 0) {
            const int64_t cap = max_notional / px;
            if (cap <= 0) return kOpsRisk;
            if (qty > cap) qty = cap;
        }
        return direct(acct_, name, side, qty, px, now_us);
    }

    int beat_basket(Basket& b, int64_t now_us, int64_t max_qty, int64_t max_notional) noexcept {
        int sent = 0;
        bool pending = false;
        for (int i = 0; i < b.n; ++i) {
            BasketLeg& leg = b.leg[i];
            const int64_t net = net_qty(leg.name);
            const int64_t gap = leg.target - net;
            if (gap == 0) continue;
            pending = true;
            if (now_us - leg.last_us < 200000) continue;
            const uint8_t side = gap > 0 ? static_cast<uint8_t>(Side::Buy) : static_cast<uint8_t>(Side::Sell);
            const int64_t qty = gap > 0 ? gap : -gap;
            const int rc = send_child(leg.name, side, qty, now_us, max_qty, max_notional);
            leg.last_us = now_us;
            if (rc == kOpsOk) ++sent;
        }
        if (!pending) b.active = 0;
        return sent;
    }

    int beat_algo(Algo& a, int64_t now_us, int64_t max_qty, int64_t max_notional) noexcept {
        if (now_us < a.next_us) return 0;
        const int64_t rem = a.qty - a.sent;
        if (rem <= 0) {
            a.active = 0;
            return 0;
        }
        int64_t per = a.qty * a.slice_us / a.horizon_us;
        if (per < 1) per = 1;
        if (per > rem) per = rem;
        const int rc = send_child(a.name, a.side, per, now_us, max_qty, max_notional);
        if (rc == kOpsOk) a.sent += per;
        a.next_us = now_us + a.slice_us;
        if (a.sent >= a.qty) a.active = 0;
        return rc == kOpsOk ? 1 : 0;
    }

    int64_t net_qty(uint32_t name) const noexcept { return net_ ? net_(net_user_, name) : 0; }

    IdMint* mint_{nullptr};
    SendFn send_{nullptr};
    void* send_user_{nullptr};
    ManualTopFn top_{nullptr};
    void* top_user_{nullptr};
    NetFn net_{nullptr};
    void* net_user_{nullptr};
    uint32_t acct_{1};
    Basket basket_[kBaskets]{};
    Algo algo_[kAlgos]{};
};

}  // namespace pitlane
