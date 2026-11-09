#include "pitlane/pitlane.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

int g_fails = 0;

void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

}  // namespace

static void check_ring() {
    pitlane::FanRing<pitlane::Intent, 4> ring;
    pitlane::Intent item{};
    item.tag = 1;
    expect(ring.publish(item, 0x3), "publish 1");
    item.tag = 2;
    expect(ring.publish(item, 0x3), "publish 2");
    item.tag = 3;
    expect(ring.publish(item, 0x3), "publish 3");
    item.tag = 4;
    expect(ring.publish(item, 0x3), "publish 4");
    item.tag = 5;
    expect(!ring.publish(item, 0x3), "full until both readers ack");

    pitlane::Intent out{};
    bool fresh = false;
    expect(ring.at(0, 0, out, &fresh) && fresh && out.tag == 1, "reader 0 sees first");
    expect(ring.ack(0, 0), "reader 0 acks");
    expect(!ring.publish(item, 0x3), "still blocked on reader 1");
    expect(ring.ack(0, 1), "reader 1 acks");
    expect(ring.publish(item, 0x3), "reuses the freed slot");

    pitlane::FanRing<pitlane::Intent, 4> claim;
    item.tag = 9;
    expect(claim.publish(item, 0), "exclusive publish");
    expect(claim.ack(0, 2), "first claim");
    expect(!claim.ack(0, 3), "second claim loses");
}

static void check_ids_steps_cash() {
    pitlane::IdMint mint(7, 2);
    const uint64_t id = mint.next(13, 35);
    expect(pitlane::id_site(id) == 7, "site");
    expect(pitlane::id_kind(id) == 2, "kind");
    expect(pitlane::id_day_mod(id) == 3, "day");
    expect(pitlane::id_ten_sec(id) == 3, "slot");
    expect(pitlane::id_serial(id) == 1, "serial");
    const uint64_t id2 = mint.next(13, 35);
    expect(pitlane::id_serial(id2) == 2, "serial grows");

    const pitlane::PxBand bands[] = {{0, 1000, 1}, {1000, 5000, 5}};
    expect(pitlane::step_floor(bands, 2, 1003) == 1000, "floor");
    expect(pitlane::step_ceil(bands, 2, 1003) == 1005, "ceil");
    expect(pitlane::step_floor(bands, 2, 1000) == 1000, "edge");

    pitlane::CashLine rows[4]{};
    pitlane::CashBoard book(rows, 4);
    pitlane::CashLine* row = book.upsert(1, "alpha", "acct", "desk", "book");
    expect(row != nullptr, "cash row");
    pitlane::CashBoard::credit(*row, 1000);
    expect(pitlane::CashBoard::reserve(*row, 400), "reserve");
    expect(row->free_amt == 600 && row->held_amt == 400, "split cash");
    expect(!pitlane::CashBoard::reserve(*row, 601), "no overdraft");
    expect(pitlane::CashBoard::commit_held(*row, 100), "commit");
    expect(row->held_amt == 300, "held after commit");
    expect(pitlane::CashBoard::release(*row, 300), "release");
    expect(row->free_amt == 900 && row->held_amt == 0, "cash restored");

    pitlane::CashBoard again(rows, 4);
    again.reindex();
    expect(again.find(pitlane::CashBoard::key_of(1, "alpha", "acct", "desk", "book")) == row, "reindex");
}

static void check_books() {
    pitlane::DepthBoard<4> depth;
    depth.insert_bid(0, 10, 1);
    depth.insert_bid(0, 11, 2);
    pitlane::RankPx view[4]{};
    depth.copy_bid(view);
    expect(view[0].px == 11 && view[0].qty == 2, "best rank");
    expect(view[1].px == 10 && view[1].qty == 1, "next rank");
    depth.erase_bid(0);
    depth.copy_bid(view);
    expect(view[0].px == 10 && view[1].px == 0, "erase shifts pointers");

    pitlane::AggSide bids(true);
    expect(bids.apply(10, 4), "agg 10");
    expect(bids.apply(12, 1), "agg 12");
    expect(bids.apply(11, 3), "agg 11");
    int64_t px = 0;
    int64_t qty = 0;
    expect(bids.top(&px, &qty) && px == 12 && qty == 1, "agg best at back");
    expect(bids.apply(12, -1), "remove best");
    expect(bids.top(&px, &qty) && px == 11, "next best");

    pitlane::CrossBook book;
    int why = 0;
    pitlane::Deal deals[8]{};
    const pitlane::CrossIn buy_a{1, 1, 10, 5, 1, 1};
    const pitlane::CrossIn buy_b{2, 2, 10, 5, 1, 1};
    const pitlane::CrossIn buy_low{4, 4, 9, 5, 1, 1};
    expect(book.submit(buy_a, deals, 8, &why) == 0 && why == 0, "rest a");
    expect(book.submit(buy_b, deals, 8, &why) == 0, "rest b");
    const pitlane::CrossIn sell{3, 3, 10, 7, 2, 1};
    const int n = book.submit(sell, deals, 8, &why);
    expect(n == 2, "two deals");
    expect(deals[0].maker == 1 && deals[0].qty == 5 && deals[0].px == 10, "first in line");
    expect(deals[1].maker == 2 && deals[1].qty == 2 && deals[1].px == 10, "second partial");
    expect(book.top_bid(&px, &qty) && px == 10 && qty == 3, "remainder rests");

    pitlane::CrossBook ladder;
    expect(ladder.submit(buy_a, deals, 8, &why) == 0, "bid 10");
    expect(ladder.submit(buy_low, deals, 8, &why) == 0, "bid 9");
    const pitlane::CrossIn lift{8, 8, 10, 5, 2, 1};
    expect(ladder.submit(lift, deals, 8, &why) == 1, "clears 10");
    expect(deals[0].px == 10 && deals[0].maker == 1, "passive price");
    expect(ladder.top_bid(&px, &qty) && px == 9 && qty == 5, "empty rung dropped");

    pitlane::CrossBook wash;
    const pitlane::CrossIn own{11, 7, 10, 4, 1, 1};
    expect(wash.submit(own, deals, 8, &why) == 0, "own bid");
    const pitlane::CrossIn hit_self{12, 7, 10, 4, 2, 1};
    expect(wash.submit(hit_self, deals, 8, &why) == 0, "no self fill");
    expect(why == static_cast<int>(pitlane::Why::Wash), "wash code");
    expect(wash.resting() == 1, "bid still there");

    pitlane::CrossBook post;
    const pitlane::CrossIn ask{21, 1, 10, 4, 2, 1};
    expect(post.submit(ask, deals, 8, &why) == 0, "rest ask");
    const pitlane::CrossIn take{22, 2, 10, 4, 1, 2};
    expect(post.submit(take, deals, 8, &why) == 0, "passive rejected");
    expect(why == static_cast<int>(pitlane::Why::Mode), "passive code");
    expect(post.resting() == 1, "ask untouched");

    int64_t pulled = 0;
    expect(post.withdraw(21, &pulled) && pulled == 4, "withdraw");
    expect(!post.top_ask(&px, &qty), "book empty");

    const pitlane::CallIn calls[] = {
        {1, 100, 10, 1},
        {2, 101, 10, 1},
        {3, 99, 10, 2},
        {4, 101, 10, 2},
    };
    pitlane::CallOut fills[4]{};
    int64_t matched = 0;
    const int64_t px_clear = pitlane::auction_clear(calls, 4, 100, 1, fills, 4, &matched);
    expect(px_clear == 101 && matched == 10, "call price");
    expect(fills[0].buy_tag == 2 && fills[0].sell_tag == 3 && fills[0].qty == 10, "call fifo");
}

static void check_gap_arrive_lag() {
    pitlane::Gap gap;
    pitlane::gap_join(gap, 50);
    expect(pitlane::gap_print(gap, 10, 1, 4) == 0, "still behind");
    expect(pitlane::gap_print(gap, 50, 1, 4) == 4, "overshoot capped by leaves");

    pitlane::Gap sized;
    pitlane::gap_join(sized, 50);
    expect(pitlane::gap_resize(sized, 50, 40, 1.0, 1, 4) == 0, "cancel from the front");
    expect(sized.ahead == 40, "ahead follows display");

    const pitlane::Rung asks[] = {{100, 5}, {101, 5}};
    const pitlane::ArrivalHit day = pitlane::walk_buy(pitlane::Style::Day, 101, 20, asks, 2);
    expect(day.kind == pitlane::Arrival::Take && day.filled == 20, "day fills the limit rung");
    const pitlane::ArrivalHit passive = pitlane::walk_buy(pitlane::Style::Passive, 101, 20, asks, 2);
    expect(passive.kind == pitlane::Arrival::Drop && passive.filled == 0, "passive expires");
    const pitlane::ArrivalHit rest = pitlane::walk_buy(pitlane::Style::Passive, 99, 20, asks, 2);
    expect(rest.kind == pitlane::Arrival::Rest && rest.filled == 0, "passive rests");
    const pitlane::ArrivalHit ioc = pitlane::walk_buy(pitlane::Style::Immediate, 101, 12, asks, 2);
    expect(ioc.kind == pitlane::Arrival::Drop && ioc.filled == 10, "immediate keeps shown size");
    const pitlane::ArrivalHit fok = pitlane::walk_buy(pitlane::Style::All, 101, 20, asks, 2);
    expect(fok.kind == pitlane::Arrival::Drop && fok.filled == 0, "all-or-nothing short");
    const pitlane::ArrivalHit fok_ok = pitlane::walk_buy(pitlane::Style::All, 101, 8, asks, 2);
    expect(fok_ok.kind == pitlane::Arrival::Take && fok_ok.filled == 8, "all-or-nothing covered");

    expect(pitlane::session_band(9 * 60) < 0, "before the open");
    expect(pitlane::session_band(9 * 60 + 40) == 0, "first band");
    expect(pitlane::session_band(10 * 60) == 1, "ten o'clock");
    expect(pitlane::inside_span(2330, 2100, 230), "overnight window");
    expect(!pitlane::send_room(true, false, 0, 3, 99, 100, 90, 1), "too close to the limit");

    const pitlane::LagPoint pts[] = {{100, 110, 130}, {200, 0, 250}, {300, 340, 360}};
    int cursor = 0;
    const int64_t rejected = pitlane::go_delay(pts, 3, cursor, 150);
    expect(rejected < 0, "reject path is negative");
    cursor = 0;
    const int64_t back = pitlane::back_delay(pts, 3, cursor, 110);
    expect(back == 20, "reply lag");

    pitlane::TapeStamp rows[] = {{100, 90}, {200, 195}};
    const int64_t shift = pitlane::lift_locals(rows, 2, 5);
    expect(shift == 15, "lift");
    expect(rows[0].local_ns - rows[0].exch_ns == 5, "new minimum");

    pitlane::NetHold hold{1, 2, 0, 0};
    const pitlane::Move flat = pitlane::one_move(0, hold, 10, 11, true);
    expect(flat.kind == pitlane::MoveKind::SellCloseYday && flat.qty == 2 && flat.px == 10, "close yesterday first");
    const pitlane::Move grow = pitlane::one_move(5, hold, 10, 11, true);
    expect(grow.kind == pitlane::MoveKind::BuyOpen && grow.qty == 2 && grow.px == 11, "open the gap");

    pitlane::LinearBook lin{2, 0, 0, 0};
    expect(lin.notion(10, 3) == 60, "linear notion");
    pitlane::apply_fill(lin, true, 10, 3, 1);
    expect(lin.pos == 3 && lin.cash == -60 && lin.equity(10) == -1, "long equity");
}

static void check_venue_and_region() {
    pitlane::VenueCfg cfg{};
    cfg.lot = 1;
    cfg.lag.go_us[0] = 5000000;
    cfg.lag.pull_us[0] = 2000000;
    cfg.rate_cap = 2;
    pitlane::PaperVenue venue(cfg);
    const int64_t stamp = (9 * 60 + 40) * 60 * 1000000ll;
    const pitlane::Rung asks[] = {{100, 2}};
    venue.set_levels(1, nullptr, 0, asks, 1);
    venue.set_band(1, 90, 110);

    pitlane::Intent buy{};
    buy.tag = 5;
    buy.name = 1;
    buy.acct = 3;
    buy.px = 100;
    buy.qty = 4;
    buy.stamp_us = stamp;
    buy.side = 1;
    buy.act = 1;
    buy.style = 1;
    pitlane::Report reps[8]{};
    pitlane::VenueOut staged = venue.admit(buy, stamp, reps, 8);
    expect(staged.why == pitlane::Why::None && staged.reports == 1, "staged");
    expect(reps[0].phase == static_cast<uint8_t>(pitlane::Phase::Staged), "not live yet");
    expect(venue.on_clock(stamp + 1000, reps, 8) == 0, "lag not elapsed");
    const int n = venue.on_clock(stamp + 5000000, reps, 8);
    expect(n == 1 && reps[0].phase == static_cast<uint8_t>(pitlane::Phase::Filled) && reps[0].qty == 4, "day fills past shown size");

    pitlane::PaperVenue rest_v(pitlane::VenueCfg{});
    const pitlane::Rung high_ask[] = {{130, 10}};
    const pitlane::Rung bid[] = {{100, 50}};
    rest_v.set_levels(1, bid, 1, high_ask, 1);
    pitlane::Intent bid_ord = buy;
    bid_ord.tag = 8;
    bid_ord.px = 100;
    bid_ord.qty = 4;
    bid_ord.stamp_us = stamp;
    pitlane::VenueOut rested = rest_v.admit(bid_ord, stamp + 1, reps, 8);
    expect(rested.why == pitlane::Why::None && reps[0].phase == static_cast<uint8_t>(pitlane::Phase::Working), "rests behind size");
    expect(rest_v.on_print(1, 100, 10, stamp + 2, reps, 8) == 0, "queue still ahead");
    expect(rest_v.on_print(1, 100, 50, stamp + 3, reps, 8) == 1 && reps[0].qty == 4, "print reaches the order");

    pitlane::PaperVenue passive(pitlane::VenueCfg{});
    passive.set_levels(1, nullptr, 0, asks, 1);
    pitlane::Intent pk = buy;
    pk.tag = 9;
    pk.style = 2;
    pk.stamp_us = stamp;
    pitlane::VenueOut denied = passive.admit(pk, stamp + 1, reps, 8);
    expect(denied.why == pitlane::Why::None && reps[0].phase == static_cast<uint8_t>(pitlane::Phase::Expired), "passive cross expires");

    pitlane::Intent again = buy;
    again.tag = 6;
    again.px = 120;
    pitlane::VenueOut band = venue.admit(again, stamp, reps, 8);
    expect(band.why == pitlane::Why::Band, "outside the band");

    pitlane::PaperVenue wash(pitlane::VenueCfg{});
    pitlane::Intent sell{};
    sell.tag = 15;
    sell.name = 2;
    sell.acct = 9;
    sell.px = 100;
    sell.qty = 3;
    sell.stamp_us = stamp;
    sell.side = 2;
    sell.act = 1;
    sell.style = 1;
    expect(wash.admit(sell, stamp, reps, 8).why == pitlane::Why::None, "sell rests");
    pitlane::Intent hit = sell;
    hit.tag = 16;
    hit.side = 1;
    expect(wash.admit(hit, stamp, reps, 8).why == pitlane::Why::Wash, "same account");

    const char* path = "pitlane_region.bin";
    ::unlink(path);
    pitlane::MapInfo map{};
    expect(pitlane::map_create(path, pitlane::desk_bytes(), &map) == 0, "map");
    pitlane::Desk* desk = pitlane::desk_view(&map, 1);
    expect(desk != nullptr, "format");
    pitlane::Intent wire{};
    wire.tag = 42;
    wire.qty = 1;
    expect(desk->intents.publish(wire, 0x1), "shm publish");
    pitlane::map_release(&map);

    pitlane::MapInfo again_map{};
    expect(pitlane::map_attach(path, pitlane::desk_bytes(), &again_map) == 0, "remap");
    pitlane::Desk* back = pitlane::desk_view(&again_map, 0);
    expect(back != nullptr, "magic");
    pitlane::Intent got{};
    bool fresh = false;
    expect(back->intents.at(0, 0, got, &fresh) && fresh && got.tag == 42, "shm read");
    pitlane::map_release(&again_map);
    ::unlink(path);
}

int main() {
    expect(pitlane::kVersionMajor == 2 && pitlane::kVersionMinor == 0 && pitlane::kVersionPatch == 0, "release");
    expect(std::strcmp(pitlane::kVersion, "2.0.0") == 0, "release text");
    check_ring();
    check_ids_steps_cash();
    check_books();
    check_gap_arrive_lag();
    check_venue_and_region();
    if (g_fails) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
