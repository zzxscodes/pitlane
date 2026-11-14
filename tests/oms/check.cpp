#include "pitlane/order/ems.hpp"
#include "pitlane/order/oms.hpp"
#include "pitlane/order/pms.hpp"
#include "pitlane/order/rms.hpp"
#include "pitlane/order/sor.hpp"

#include <cstdint>
#include <cstdio>

static int g_fails = 0;

static void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

static void check_oms() {
    pitlane::Oms<16> oms;
    pitlane::OmsOrder d{};
    d.local = 1;
    d.client = 11;
    d.parent = 7;
    d.name = 1;
    d.acct = 1;
    d.px = 10;
    d.qty = 100;
    d.side = 1;
    d.created_us = 5;
    d.updated_us = 5;
    expect(oms.submit(d), "oms submit");
    const pitlane::OmsOrder* o = oms.find_local(1);
    expect(o && o->phase == static_cast<uint8_t>(pitlane::Phase::Staged) && o->leaves == 100 && o->version == 1 &&
               o->filled == 0 && o->avg_px == 0,
           "oms staged");
    expect(!oms.submit(d), "oms duplicate submit");
    expect(oms.ack(1, 900, 3, 10) && oms.find_local(1)->phase == static_cast<uint8_t>(pitlane::Phase::Working) &&
               oms.find_local(1)->venue == 3 && oms.find_local(1)->venue_id == 900,
           "oms ack");
    expect(oms.find_client(11) == oms.find_local(1) && oms.find_venue(900) == oms.find_local(1), "oms lookups");

    pitlane::OmsFill f{};
    expect(oms.fill(1, 500, 40, 10, 11, f) && f.qty == 40 && f.leaves == 60 && f.avg_px == 10 && f.dup == 0 &&
               f.phase == static_cast<uint8_t>(pitlane::Phase::Partial),
           "oms partial fill");
    expect(oms.find_local(1)->filled == 40 && oms.find_local(1)->avg_px == 10, "oms partial state");
    expect(!oms.fill(1, 500, 40, 10, 12, f) && f.dup == 1, "oms duplicate fill");
    expect(oms.find_local(1)->filled == 40 && oms.find_local(1)->leaves == 60, "oms duplicate unchanged");
    expect(oms.fill(1, 501, 60, 11, 13, f) && f.leaves == 0 && f.avg_px == 10 &&
               f.phase == static_cast<uint8_t>(pitlane::Phase::Filled),
           "oms filled");
    expect(oms.find_local(1)->filled == 100 && oms.find_local(1)->leaves == 0, "oms filled state");

    pitlane::OmsOrder d2{};
    d2.local = 2;
    d2.qty = 10;
    d2.px = 10;
    d2.side = 1;
    expect(oms.submit(d2) && oms.ack(2, 901, 3, 20), "oms second submit");
    expect(!oms.fill(2, 600, 11, 10, 21, f), "oms overfill");

    pitlane::OmsOrder d3{};
    d3.local = 3;
    d3.qty = 100;
    d3.px = 20;
    d3.side = 1;
    expect(oms.submit(d3) && oms.ack(3, 902, 3, 30), "oms third submit");
    pitlane::OmsOrder before{};
    expect(oms.amend(3, 21, 120, 2, before) && before.px == 20 && before.qty == 100, "oms amend");
    expect(oms.find_local(3)->px == 21 && oms.find_local(3)->qty == 120 && oms.find_local(3)->leaves == 120 &&
               oms.find_local(3)->version == 3,
           "oms amended state");
    expect(!oms.amend(3, 22, 100, 2, before), "oms stale version");

    expect(oms.cancel(2, 40) && oms.find_local(2)->phase == static_cast<uint8_t>(pitlane::Phase::PullWait),
           "oms pull");
    expect(oms.confirm_cancel(2, 41) &&
               oms.find_local(2)->phase == static_cast<uint8_t>(pitlane::Phase::Cancelled),
           "oms cancelled");

    pitlane::OmsOrder d4{};
    d4.local = 4;
    d4.qty = 5;
    d4.px = 10;
    d4.side = 2;
    expect(oms.submit(d4) && oms.reject(4, 50) &&
               oms.find_local(4)->phase == static_cast<uint8_t>(pitlane::Phase::Denied),
           "oms reject");

    pitlane::Oms<8> prog;
    pitlane::OmsOrder p1{};
    p1.local = 100;
    p1.parent = 7;
    p1.qty = 10;
    p1.px = 10;
    p1.side = 1;
    pitlane::OmsOrder p2{};
    p2.local = 101;
    p2.parent = 7;
    p2.qty = 5;
    p2.px = 10;
    p2.side = 1;
    expect(prog.submit(p1) && prog.submit(p2) && prog.ack(100, 1, 1, 1) && prog.ack(101, 2, 1, 1),
           "oms parent submit");
    pitlane::OmsFill pf{};
    expect(prog.fill(100, 900, 10, 10, 2, pf), "oms parent fill");
    int64_t done = 0;
    int64_t working = 0;
    int children = 0;
    expect(prog.parent_progress(7, done, working, children) && done == 10 && working == 5 && children == 2,
           "oms parent progress");

    pitlane::Oms<2> small;
    pitlane::OmsOrder s1{};
    s1.local = 1;
    s1.qty = 1;
    s1.px = 1;
    s1.side = 1;
    pitlane::OmsOrder s2 = s1;
    s2.local = 2;
    pitlane::OmsOrder s3 = s1;
    s3.local = 3;
    expect(small.submit(s1) && small.submit(s2) && !small.submit(s3), "oms capacity");
    expect(small.size() == 2 && small.active() == 2, "oms size");
}

static void check_sor() {
    using namespace pitlane;
    expect(eff_buy_px(1000, 30) == 1003, "sor eff buy");
    expect(eff_sell_px(1000, 30) == 997, "sor eff sell");
    expect(eff_buy_px(0, 10) == 0 && eff_sell_px(-1, 10) == 0, "sor eff guard");

    VenueTop v[2]{};
    v[0].venue = 1;
    v[0].bid_px = 990;
    v[0].ask_px = 1000;
    v[0].bid_qty = 100;
    v[0].ask_qty = 100;
    v[0].taker_fee_bps = 30;
    v[1].venue = 2;
    v[1].bid_px = 999;
    v[1].ask_px = 1002;
    v[1].bid_qty = 100;
    v[1].ask_qty = 100;
    v[1].taker_fee_bps = 0;
    SorSlice out[4]{};
    expect(sor_aggressive_buy(v, 2, 50, out, 4) == 1 && out[0].venue == 2 && out[0].qty == 50, "sor net venue");
    expect(sor_aggressive_sell(v, 2, 50, out, 4) == 1 && out[0].venue == 2 && out[0].qty == 50,
           "sor sell net venue");

    VenueTop c[2]{};
    c[0].venue = 1;
    c[0].bid_px = 100;
    c[0].ask_px = 100;
    c[0].bid_qty = 30;
    c[0].ask_qty = 30;
    c[1].venue = 2;
    c[1].bid_px = 99;
    c[1].ask_px = 101;
    c[1].bid_qty = 100;
    c[1].ask_qty = 100;
    int n = sor_aggressive_buy(c, 2, 100, out, 4);
    expect(n == 2 && out[0].qty == 30 && out[1].qty == 70, "sor buy cap");
    n = sor_aggressive_sell(c, 2, 100, out, 4);
    expect(n == 2 && out[0].qty == 30 && out[1].qty == 70, "sor sell cap");
    expect(sor_aggressive_buy(v, 2, 0, out, 4) == 0 && sor_aggressive_buy(nullptr, 2, 10, out, 4) == 0,
           "sor fail closed");

    VenueTop lot[1]{};
    lot[0].venue = 1;
    lot[0].ask_px = 100;
    lot[0].ask_qty = 100;
    lot[0].lot = 10;
    n = sor_aggressive_buy(lot, 1, 25, out, 4);
    expect(n == 1 && out[0].qty == 20, "sor lot round");

    VenueTop p[2]{};
    p[0].venue = 1;
    p[0].bid_px = 100;
    p[0].ask_px = 101;
    p[0].passive_fill_ppm = 1000;
    p[1].venue = 2;
    p[1].bid_px = 100;
    p[1].ask_px = 101;
    p[1].passive_fill_ppm = 2000;
    n = sor_passive(p, 2, 90, out, 4);
    expect(n == 2 && out[0].qty == 30 && out[1].qty == 60, "sor passive weight");

    ShownHold hold{};
    hold.fresh_us = 100;
    expect(hold.claim(5, 10, 1000) == 5 && hold.held == 5, "sor show claim");
    expect(hold.claim(10, 10, 1001) == 5 && hold.held == 10, "sor show room");
    expect(hold.claim(1, 10, 1002) == 0, "sor show full");
    hold.release(4);
    expect(hold.held == 6, "sor show release");
    hold.release(100);
    expect(hold.held == 0, "sor show clamp");
    expect(hold.claim(3, 10, 1200) == 3 && hold.mark_us == 1200 && hold.held == 3, "sor show fresh");
}

static void check_ems() {
    pitlane::Ems<4, 16> ems;
    expect(ems.open(7, 100, 300, 100, 1, 1000, 1), "ems open");
    expect(!ems.open(7, 100, 300, 100, 1, 1000, 1), "ems duplicate open");
    expect(!ems.open(8, 100, 300, 100, 1, 1000, 3), "ems bad side");
    pitlane::VenueTop v{};
    v.venue = 1;
    v.bid_px = 9;
    v.ask_px = 10;
    v.bid_qty = 1000;
    v.ask_qty = 1000;
    v.lot = 1;
    pitlane::SorSlice slices[4]{};
    expect(ems.poll(7, 1000, &v, 1, slices, 4) == 1 && slices[0].qty == 33, "ems first slice");
    expect(ems.poll(7, 1050, &v, 1, slices, 4) == 0, "ems too early");
    expect(ems.on_child(7, 1001, 33, true), "ems child one");
    int64_t done = 0;
    int64_t working = 0;
    expect(ems.progress(7, done, working) && done == 33 && working == 0, "ems progress one");
    expect(ems.poll(7, 1100, &v, 1, slices, 4) == 1 && slices[0].qty == 33, "ems second slice");
    expect(ems.on_child(7, 1002, 33, true), "ems child two");
    expect(ems.poll(7, 1200, &v, 1, slices, 4) == 1 && slices[0].qty == 34, "ems third slice");
    expect(ems.on_child(7, 1003, 34, true), "ems child three");
    expect(ems.progress(7, done, working) && done == 100 && working == 0, "ems complete");
    expect(ems.close(7) && ems.count() == 0, "ems close");
}

static void check_pms() {
    pitlane::Pms<8> pms;
    pitlane::PmsFill f{};
    f.acct = 1;
    f.name = 2;
    f.side = 1;
    f.qty = 100;
    f.px = 10;
    f.fee = 1;
    expect(pms.apply(f), "pms open long");
    const pitlane::PmsPos* p = pms.find(1, 2);
    expect(p && p->long_qty == 100 && p->avg_long_px == 10 && p->fees == 1, "pms long one");
    f.px = 12;
    f.fee = 2;
    expect(pms.apply(f), "pms add long");
    p = pms.find(1, 2);
    expect(p && p->long_qty == 200 && p->avg_long_px == 11 && p->fees == 3, "pms average long");

    f = pitlane::PmsFill{};
    f.acct = 1;
    f.name = 2;
    f.side = 2;
    f.qty = 50;
    f.px = 13;
    f.fee = 1;
    f.close_qty = 50;
    expect(pms.apply(f), "pms close long");
    p = pms.find(1, 2);
    expect(p && p->long_qty == 150 && p->avg_long_px == 11 && p->realized == 100 && p->fees == 4,
           "pms realized long");

    f = pitlane::PmsFill{};
    f.acct = 1;
    f.name = 2;
    f.side = 2;
    f.qty = 50;
    f.px = 20;
    expect(pms.apply(f), "pms open short");
    f = pitlane::PmsFill{};
    f.acct = 1;
    f.name = 2;
    f.side = 1;
    f.qty = 20;
    f.px = 18;
    f.close_qty = 20;
    expect(pms.apply(f), "pms close short");
    p = pms.find(1, 2);
    expect(p && p->short_qty == 30 && p->avg_short_px == 20 && p->realized == 140, "pms short state");
    expect(pms.net_qty(1, 2) == 120, "pms net");
    int64_t u = 0;
    expect(pms.unrealized(1, 2, 12, u) && u == 390, "pms unrealized");

    f = pitlane::PmsFill{};
    f.acct = 1;
    f.name = 2;
    f.side = 1;
    f.qty = 10;
    f.px = 10;
    f.close_qty = 20;
    expect(!pms.apply(f), "pms close over qty");
    f.close_qty = 5;
    f.close_yday = 6;
    expect(!pms.apply(f), "pms yday over close");
    expect(!pms.apply(pitlane::PmsFill{}), "pms empty fill");
    expect(pms.size() == 1, "pms size");
}

static void check_rms() {
    pitlane::Rms r;
    pitlane::RmsLimits lim{};
    lim.max_order_qty = 100;
    lim.max_order_notional = 10000;
    lim.max_pos_long = 100;
    lim.max_pos_short = 100;
    lim.max_daily_notional = 100000;
    lim.max_orders_per_sec = 5;
    lim.price_band_ppm = 10000;
    lim.max_live_orders = 3;
    r.set(lim);
    expect(r.limits().max_order_qty == 100 && !r.halted(), "rms set");

    pitlane::RmsReq req{};
    req.acct = 1;
    req.name = 1;
    req.side = 1;
    req.px = 100;
    req.qty = 10;
    req.last_px = 100;
    req.free_cash = 1000000;
    expect(r.check(req) == pitlane::RmsCode::Ok, "rms ok");

    pitlane::RmsReq bad = req;
    bad.qty = 0;
    expect(r.check(bad) == pitlane::RmsCode::Qty, "rms qty zero");
    bad = req;
    bad.qty = 101;
    expect(r.check(bad) == pitlane::RmsCode::Qty, "rms max qty");
    bad = req;
    bad.side = 3;
    expect(r.check(bad) == pitlane::RmsCode::Qty, "rms bad side");
    bad = req;
    bad.px = 1100;
    expect(r.check(bad) == pitlane::RmsCode::Notional, "rms notional");
    bad = req;
    bad.daily_notional = 99995;
    expect(r.check(bad) == pitlane::RmsCode::Notional, "rms daily notional");
    bad = req;
    bad.px = 102;
    expect(r.check(bad) == pitlane::RmsCode::PriceBand, "rms price band");
    bad = req;
    bad.px = 101;
    expect(r.check(bad) == pitlane::RmsCode::Ok, "rms price band edge");

    pitlane::PmsPos pos{};
    pos.long_qty = 95;
    pos.short_qty = 95;
    bad = req;
    bad.pos = &pos;
    expect(r.check(bad) == pitlane::RmsCode::Position, "rms long position");
    bad.side = 2;
    expect(r.check(bad) == pitlane::RmsCode::Position, "rms short position");
    bad = req;
    bad.live_orders = 3;
    expect(r.check(bad) == pitlane::RmsCode::OpenOrders, "rms live orders");
    bad = req;
    bad.orders_in_window = 5;
    expect(r.check(bad) == pitlane::RmsCode::Rate, "rms rate");
    bad = req;
    bad.free_cash = 500;
    expect(r.check(bad) == pitlane::RmsCode::Cash, "rms cash");

    r.halt(true);
    expect(r.halted() && r.check(req) == pitlane::RmsCode::Halted, "rms halted");
    r.halt(false);
    expect(!r.halted() && r.check(req) == pitlane::RmsCode::Ok, "rms resumed");
}

int main() {
    check_oms();
    check_sor();
    check_ems();
    check_pms();
    check_rms();
    if (g_fails) {
        std::fprintf(stderr, "%d oms check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
