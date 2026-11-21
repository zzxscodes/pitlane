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
               Off off = Off::Open, uint64_t* tag_out = nullptr, Biz biz = Biz::Trade) noexcept {
        if (!biz_directional(biz) || !known_side(side) || qty <= 0 || name == 0) return kOpsBadReq;
        if (!off_known(off)) return kOpsBadReq;
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
        in.off = off;
        in.biz = static_cast<uint8_t>(biz);
        const int rc = send_ ? send_(send_user_, in) : 2;
        if (rc == 0 && tag_out) *tag_out = tag;
        return rc == 0 ? kOpsOk : (rc == 1 ? kOpsRisk : kOpsBusy);
    }

    // Target positions. A negative target is a short or a full close.
    int basket_open(uint64_t id, const OpsLeg* legs, int n, int64_t now_us, uint32_t acct = 0) noexcept {
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
        b.acct = acct ? acct : acct_;
        b.n = static_cast<uint8_t>(n);
        for (int i = 0; i < n; ++i) {
            b.leg[i].name = legs[i].name;
            b.leg[i].target = legs[i].target;
            b.leg[i].last_us = now_us - 200000;
        }
        b.active = 1;
        return kOpsOk;
    }

    // mode 0 slices the parent on one side; mode 1 is a T0 round trip that
    // opens `qty` and then closes it back to the position it started from.
    int algo_open(uint64_t id, uint32_t name, uint8_t side, int64_t qty, int64_t horizon_us, int64_t slice_us,
                  int64_t now_us, uint32_t acct = 0, uint8_t mode = 0) noexcept {
        if (id == 0 || name == 0 || !known_side(side) || qty <= 0) return kOpsBadReq;
        if (horizon_us <= 0 || slice_us <= 0 || slice_us > horizon_us) return kOpsBadReq;
        if (mode > 1) return kOpsBadReq;
        if (mode == 1 && !net_) return kOpsBadReq;  // a T0 needs the shared position
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
        a.acct = acct ? acct : acct_;
        a.name = name;
        a.side = side;
        a.qty = qty;
        a.horizon_us = horizon_us;
        a.slice_us = slice_us;
        a.start_us = now_us;
        a.next_us = now_us;
        a.mode = mode;
        a.start_net = (mode == 1 && net_) ? net_(net_user_, name) : 0;
        a.active = 1;
        return kOpsOk;
    }

    // Per-algo lifecycle: pause stops new slices, resume restarts the clock,
    // cancel abandons the parent, and update changes the remaining schedule.
    // Unknown or finished ids are refused.
    int algo_pause(uint64_t id) noexcept {
        Algo* a = find_algo(id);
        if (!a) return kOpsBadReq;
        a->paused = 1;
        return kOpsOk;
    }

    int algo_resume(uint64_t id, int64_t now_us) noexcept {
        Algo* a = find_algo(id);
        if (!a) return kOpsBadReq;
        a->paused = 0;
        a->next_us = now_us;
        return kOpsOk;
    }

    int algo_cancel(uint64_t id) noexcept {
        Algo* a = find_algo(id);
        if (!a) return kOpsBadReq;
        a->active = 0;
        return kOpsOk;
    }

    // Zero keeps the current quantity, horizon, or slice.
    int algo_update(uint64_t id, int64_t qty, int64_t horizon_us, int64_t slice_us, int64_t now_us) noexcept {
        Algo* a = find_algo(id);
        if (!a) return kOpsBadReq;
        const int64_t next_qty = qty > 0 ? qty : a->qty;
        if (next_qty < 1) return kOpsBadReq;
        const int64_t next_h = horizon_us > 0 ? horizon_us : a->horizon_us;
        const int64_t next_s = slice_us > 0 ? slice_us : a->slice_us;
        if (next_h <= 0 || next_s <= 0 || next_s > next_h) return kOpsBadReq;
        a->qty = next_qty;
        a->horizon_us = next_h;
        a->slice_us = next_s;
        a->start_us = now_us;
        a->next_us = now_us;
        if (a->mode == 1) {
            // Re-anchor a T0 so the new size is measured from the position the
            // account holds now, and open it again before unwinding.
            a->phase = 0;
            a->start_net = net_qty(a->name);
        }
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
        uint32_t acct{0};
        uint8_t active{0};
        uint8_t n{0};
        BasketLeg leg[kOpsBasketLegs]{};
    };

    struct Algo {
        uint64_t id{0};
        uint32_t acct{0};
        uint32_t name{0};
        uint8_t side{0};
        uint8_t active{0};
        uint8_t paused{0};
        uint8_t mode{0};
        uint8_t phase{0};
        int64_t start_net{0};
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

    // A child that reduces the signed net is a close, so a counter does not
    // turn it into an opposite-side open. The child is capped to the position
    // it can close; anything beyond opens on a later beat.
    static Off reduce_off(int64_t net, uint8_t side) noexcept {
        if (sell_side(side) && net > 0) return Off::Close;
        if (buy_side(side) && net < 0) return Off::Close;
        return Off::Open;
    }

    static int64_t close_cap(int64_t net, uint8_t side) noexcept { return sell_side(side) ? net : -net; }

    int send_child(uint32_t acct, uint32_t name, uint8_t side, int64_t qty, int64_t now_us, int64_t max_qty,
                   int64_t max_notional, Off off = Off::Open) noexcept {
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
        if (off != Off::Open && qty > close_cap(net_qty(name), side)) return kOpsBadReq;
        return direct(acct ? acct : acct_, name, side, qty, px, now_us, off);
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
            int64_t qty = gap > 0 ? gap : -gap;
            const Off off = reduce_off(net, side);
            if (off != Off::Open) {
                const int64_t cap = close_cap(net, side);
                if (qty > cap) qty = cap;
            }
            const int rc = send_child(b.acct, leg.name, side, qty, now_us, max_qty, max_notional, off);
            leg.last_us = now_us;
            if (rc == kOpsOk) ++sent;
        }
        if (!pending) b.active = 0;
        return sent;
    }

    int beat_algo(Algo& a, int64_t now_us, int64_t max_qty, int64_t max_notional) noexcept {
        if (a.paused) return 0;
        if (now_us < a.next_us) return 0;
        if (now_us - a.start_us > a.horizon_us) {
            if (a.mode == 0) {
                a.active = 0;
                return 0;
            }
            if (a.phase == 0) {
                // Out of time while opening: stop adding and unwind what is
                // there. Once closing, the parent runs until the entry net is
                // restored, because abandoning here would leave a position.
                a.phase = 1;
                a.start_us = now_us;
                a.next_us = now_us;
            }
        }
        int64_t per = a.qty * a.slice_us / a.horizon_us;
        if (per < 1) per = 1;
        if (a.mode == 1) return beat_t0(a, per, now_us, max_qty, max_notional);
        const int64_t rem = a.qty - a.sent;
        if (rem <= 0) {
            a.active = 0;
            return 0;
        }
        if (per > rem) per = rem;
        const int64_t net = net_qty(a.name);
        const Off off = reduce_off(net, a.side);
        if (off != Off::Open) {
            const int64_t cap = close_cap(net, a.side);
            if (per > cap) per = cap;
        }
        if (per <= 0) {
            a.next_us = now_us + a.slice_us;
            return 0;
        }
        const int rc = send_child(a.acct, a.name, a.side, per, now_us, max_qty, max_notional, off);
        if (rc == kOpsOk) a.sent += per;
        a.next_us = now_us + a.slice_us;
        if (a.sent >= a.qty) a.active = 0;
        return rc == kOpsOk ? 1 : 0;
    }

    // T0 round trip: work the position from the entry net to entry+qty (buy)
    // or entry-qty (sell), then unwind it back to the entry net. Progress is
    // read from the shared net, so unfilled slices are retried.
    int beat_t0(Algo& a, int64_t per, int64_t now_us, int64_t max_qty, int64_t max_notional) noexcept {
        const int64_t pos = net_qty(a.name);
        const int64_t dir = a.side == static_cast<uint8_t>(Side::Buy) ? 1 : -1;
        const int64_t target = a.start_net + dir * a.qty;
        int64_t gap = 0;
        uint8_t side = a.side;
        if (a.phase == 0) {
            gap = (target - pos) * dir;
            if (gap <= 0) {
                a.phase = 1;
                a.start_us = now_us;  // the unwind gets its own horizon
                a.next_us = now_us;
                return 0;
            }
            side = a.side;
        } else {
            gap = (pos - a.start_net) * dir;
            if (gap <= 0) {
                a.active = 0;
                return 0;
            }
            side = dir == 1 ? static_cast<uint8_t>(Side::Sell) : static_cast<uint8_t>(Side::Buy);
        }
        if (per > gap) per = gap;
        const Off off = reduce_off(pos, side);
        if (off != Off::Open) {
            const int64_t cap = close_cap(pos, side);
            if (per > cap) per = cap;
        }
        const int rc = send_child(a.acct, a.name, side, per, now_us, max_qty, max_notional, off);
        if (rc == kOpsOk) a.sent += per;
        a.next_us = now_us + a.slice_us;
        return rc == kOpsOk ? 1 : 0;
    }

    Algo* find_algo(uint64_t id) noexcept {
        for (int i = 0; i < kAlgos; ++i) {
            if (algo_[i].active && algo_[i].id == id) return &algo_[i];
        }
        return nullptr;
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
