#include "pitlane/pitlane.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

static int g_fails = 0;

static void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

static void check_steps() {
    const pitlane::PxBand bands[] = {{0, 1000, 1}, {1000, 5000, 5}};
    expect(pitlane::step_floor(bands, 2, 1003) == 1000, "floor");
    expect(pitlane::step_ceil(bands, 2, 1003) == 1005, "ceil");
    expect(pitlane::step_floor(bands, 2, 1000) == 1000, "edge");
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

static void check_gap() {
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
}

static void check_venue() {
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

    pitlane::VenueCfg closed_cfg{};
    closed_cfg.trade_open = 0;
    pitlane::PaperVenue closed(closed_cfg);
    expect(closed.admit(buy, stamp, reps, 8).why == pitlane::Why::Closed, "session closed");

    pitlane::PaperVenue drill(pitlane::VenueCfg{});
    drill.set_levels(1, bid, 1, high_ask, 1);
    pitlane::Intent dord = buy;
    dord.tag = 77;
    dord.px = 100;
    dord.qty = 4;
    expect(drill.admit(dord, stamp, reps, 8).why == pitlane::Why::None, "drill admit");
    expect(drill.hang(77, 1), "hang");
    expect(drill.on_clock(stamp + 6000000, reps, 8) == 0, "hang skips clock");
    expect(drill.force_fill(77, 100, 0, stamp + 7, reps, 8) == 1 && reps[0].phase == static_cast<uint8_t>(pitlane::Phase::Filled),
           "force fill");

    pitlane::DrillBook book;
    expect(book.set(88, pitlane::DrillCue::Kill) && book.find(88) != nullptr, "drill book");
    pitlane::TouchView tv{};
    tv.bid_px = 99;
    tv.ask_px = 101;
    tv.last_px = 100;
    expect(pitlane::resolve_px(true, pitlane::TouchMode::Opp, tv) == 101, "touch opp");
    expect(pitlane::resolve_px(true, pitlane::TouchMode::Mid, tv) == 100, "touch mid");
    expect(pitlane::tape_keep(1, 1, true) == false && pitlane::tape_keep(1, 2, true), "tape keep");

    pitlane::HoldLine hold{};
    hold.long_yday = 10;
    hold.frozen_yday = 3;
    expect(pitlane::sellable_of(hold) == 7, "sellable yday");
    hold.sellable = 4;
    expect(pitlane::sellable_of(hold) == 4 && pitlane::sell_room(hold, 5) == pitlane::Cause::NoPos, "sell room");
}

static void check_calendar() {
    expect(pitlane::ymd_weekday(20240101) == 1, "monday");
    expect(pitlane::is_weekend(20240106), "saturday");
    expect(pitlane::next_session_day(20240105, nullptr) == 20240108, "skip weekend");
    pitlane::ClosedDays closed;
    expect(closed.add(20240108), "holiday");
    expect(pitlane::next_session_day(20240105, &closed) == 20240109, "skip holiday");
    expect(pitlane::session_gap(20240105, 20240109, &closed) == 1, "one session between");
    expect(pitlane::night_back_days(190000, 1) == 3, "monday night");
    expect(pitlane::night_back_days(100000, 3) == 0, "day session");
    expect(pitlane::night_back_days(20000, 1) == 2, "monday morning");
    pitlane::Span spans[4]{};
    const int ns = pitlane::future_spans(true, 21 * 60, 150, spans, 4);
    expect(ns == 4 && pitlane::minute_in_spans(23 * 60, spans, ns), "night span");
    expect(pitlane::minute_in_spans(60, spans, ns), "after midnight");
    pitlane::SessionRow row{};
    row.open_min = 9 * 60 + 30;
    row.close_min = 15 * 60;
    row.lunch0 = 11 * 60 + 30;
    row.lunch1 = 13 * 60;
    expect(pitlane::minute_tradable(row, 10 * 60) && !pitlane::minute_tradable(row, 12 * 60), "lunch");
}

void check_quote() {
    pitlane::QuoteFold<8> book;
    pitlane::FoldEdit bad[] = {{pitlane::FoldOp::Add, 1, 10, 1}, {pitlane::FoldOp::Add, 1, 0, 1}};
    expect(!book.apply(bad, 2) && !book.built() && book.bid_count() == 0, "bad batch leaves the book");

    pitlane::FoldEdit edits[] = {
        {pitlane::FoldOp::Add, 1, 10, 4},
        {pitlane::FoldOp::Add, 1, 11, 2},
        {pitlane::FoldOp::Add, 2, 12, 3},
        {pitlane::FoldOp::Set, 2, 12, 5},
    };
    expect(book.apply(edits, 4), "apply book");
    pitlane::FoldLevel top[4]{};
    expect(book.copy(1, top, 4) == 2 && top[0].px == 11 && top[0].qty == 2 && top[1].px == 10, "bid best first");
    expect(book.copy(2, top, 4) == 1 && top[0].px == 12 && top[0].qty == 5, "ask set");

    pitlane::FoldEdit again[] = {{pitlane::FoldOp::Add, 1, 11, 1}};
    expect(!book.apply(again, 1) && book.bid_count() == 2, "add refuses an existing price");
    pitlane::FoldEdit drop[] = {{pitlane::FoldOp::Del, 1, 11, 0}};
    expect(book.apply(drop, 1) && book.bid_count() == 1, "delete");

    pitlane::PrintLog<4> prints;
    expect(prints.push(12, 1, 2), "print");
    expect(book.copy(2, top, 4) == 1 && top[0].qty == 5, "print does not touch the book");

    pitlane::DayRoll<4> roll;
    expect(roll.on_msg(60ull * 1000000ull, 10, 1), "minute");
    expect(roll.on_msg(60ull * 1000000ull + 10, 12, 2), "same minute");
    expect(!roll.on_msg(10, 99, 1), "stale clock");
    uint64_t minute = 0;
    int64_t hi = 0, lo = 0, vol = 0;
    expect(roll.at(0, &minute, &hi, &lo, &vol) && hi == 12 && lo == 10 && vol == 3, "bar");
    expect(roll.on_msg(60ull * 1000000ull + 86400000000ull, 8, 1), "next day");
    expect(roll.size() == 1 && roll.at(0, &minute, &hi, &lo, &vol) && hi == 8, "old minute dropped");
}

void check_walk() {
    const pitlane::PxBand bands[] = {{0, 1000, 1}, {1000, 5000, 5}};
    expect(pitlane::band_at_hold(bands, 2, 1000) == 0, "boundary stays on the lower band");
    expect(pitlane::walk_up(bands, 2, 998, 3, 0, 0) == 1005, "cross a band upward");
    expect(pitlane::walk_down(bands, 2, 1005, 2, 0, 0) == 999, "cross a band downward");
    expect(pitlane::walk_up(bands, 2, 990, 100, 0, 1000) == 1000, "clamp high");
    expect(pitlane::walk_down(bands, 2, 3, 10, 1, 1000) == 1, "clamp low");

    pitlane::TickGrid grid{};
    expect(grid.load(bands, 2, 0, 10000), "tick grid load");
    expect(grid.floor_px(1003) == 1000 && grid.ceil_px(1003) == 1005, "tick grid snap");
    expect(grid.walk_up(998, 3) == 1005 && grid.cursor == 1, "tick grid cursor");

    pitlane::Span spans[4]{};
    const int n = pitlane::future_spans(true, 21 * 60, 23 * 60, spans, 4);
    expect(n == 4, "future spans");
    expect(pitlane::span_index(21 * 60 + 30, spans, n) == 0, "night span");
    expect(pitlane::span_index(12 * 60, spans, n) < 0, "between sessions");
    pitlane::SessionRow row{};
    row.open_min = 9 * 60 + 30;
    row.close_min = 15 * 60;
    row.lunch0 = 11 * 60 + 30;
    row.lunch1 = 13 * 60;
    expect(pitlane::session_part(row, 12 * 60) == pitlane::SessionPart::Lunch, "lunch");
    expect(pitlane::session_part(row, 10 * 60) == pitlane::SessionPart::Open, "morning");
}

static void check_bar() {
    pitlane::BarFold bars(60);
    pitlane::Bar closed{};
    expect(bars.on_tick(10, 100, 1, true, &closed) && bars.current().open == 100, "open bar");
    expect(bars.on_tick(20, 110, 2, true, &closed) && bars.current().high == 110 && bars.current().vol == 3, "update");
    expect(!bars.on_tick(30, 90, 1, false, &closed) && bars.current().close == 110, "session flag skips the tick");
    expect(bars.on_tick(70, 80, 1, true, &closed) && closed.open == 100 && closed.close == 110, "next bucket");
    expect(bars.current().start_us == 60 && bars.current().open == 80, "new bar");
}

void check_window() {
    pitlane::PriceWindow<128> book;
    expect(!book.set(1, 100, 1), "unbound");
    expect(book.reset(100, 2), "reset");
    expect(book.set(1, 100, 3) && book.set(1, 104, 1) && book.set(2, 110, 4) && book.set(2, 108, 2), "set");
    int64_t px = 0;
    int64_t qty = 0;
    expect(book.best(1, px, qty) && px == 104 && qty == 1, "best bid");
    expect(book.set(1, 228, 9) && book.best(1, px, qty) && px == 228 && qty == 9, "far bid");
    expect(book.set(1, 228, 0) && book.best(1, px, qty) && px == 104, "far cleared");
    expect(book.set(1, 104, 0), "clear");
    expect(book.best(1, px, qty) && px == 100 && qty == 3, "next bid");
    expect(book.best(2, px, qty) && px == 108 && qty == 2, "best ask");
    expect(!book.set(1, 101, 1) && book.best(1, px, qty) && px == 100, "off step");
    expect(!book.set(1, 100 + 2 * 128, 1), "outside");
    int64_t before = -1;
    expect(book.shown_before(1, 100, before) && before == 0, "nothing above the bid");
    expect(book.set(1, 104, 1) && book.shown_before(1, 100, before) && before == 1, "one better bid");
    expect(book.shown_before(2, 110, before) && before == 2, "size under the ask");
    expect(book.shown_before(2, 108, before) && before == 0, "ask is the best");
    expect(!book.shown_before(1, 101, before) && before == 0, "off step keeps the previous sum");

    pitlane::PriceWindow<64> wide;
    expect(wide.reset(100, 1), "wide reset");
    const int64_t half = INT64_MAX / 2 + 1;
    expect(wide.set(1, 101, half) && wide.set(1, 102, half), "two large bids");
    expect(!wide.shown_before(1, 100, before), "sum overflow");
}

static void check_cover() {
    const int64_t px[] = {100, 102, 99};
    const int64_t qty[] = {5, 1, 4};
    expect(pitlane::cover_qty(px, qty, 3, true, 101, 10) == 9, "buy cover");
    expect(pitlane::cover_qty(px, qty, 3, false, 101, 10) == 1, "sell cover");
    expect(pitlane::cover_qty(px, qty, 3, true, 100, 3) == 3, "capped");
    const int64_t wide_px[] = {100, 101, 102, 103, 104, 105, 90, 91};
    const int64_t wide_qty[] = {1, 1, 1, 1, 1, 1, 2, 2};
    expect(pitlane::cover_qty(wide_px, wide_qty, 8, true, 103, 100) == 8, "buy lane");
    expect(pitlane::cover_qty(wide_px, wide_qty, 8, false, 104, 100) == 2, "sell lane");
}

static void check_target() {
    expect(pitlane::stored_target(4, 9, false, true) == 4 && pitlane::stored_target(4, 9, true, false) == 0, "target");
}

static void check_session_book() {
    const int64_t morning = (9 * 60 + 40) * 60 * 1000000ll;
    pitlane::PaperVenue venue(pitlane::VenueCfg{});
    const pitlane::Rung asks[] = {{130, 8}};
    venue.set_levels(1, nullptr, 0, asks, 1);
    pitlane::Intent buy{};
    buy.tag = 9;
    buy.name = 1;
    buy.acct = 2;
    buy.px = 100;
    buy.qty = 4;
    buy.stamp_us = morning;
    buy.side = 1;
    buy.act = 1;
    buy.style = 1;
    pitlane::Report reps[4]{};
    expect(venue.admit(buy, morning, reps, 4).why == pitlane::Why::None, "rest buy");
    expect(venue.on_tape(1, 100, 4, 1, morning + 1, reps, 4) == 0, "same-side print ignored");
    expect(venue.on_tape(1, 100, 4, 2, morning + 2, reps, 4) == 1 && reps[0].qty == 4, "opposite print fills");

    const pitlane::Rung bids[] = {{100, 3}, {99, 5}};
    const pitlane::ArrivalHit sold = pitlane::walk_sell(pitlane::Style::Immediate, 99, 10, bids, 2);
    expect(sold.kind == pitlane::Arrival::Drop && sold.filled == 8, "sell walks down");
    pitlane::InverseBook inv{10, 0, 0, 0};
    expect(inv.notion(5, 20) == 40, "inverse notion");
    const pitlane::CallIn tie[] = {{1, 12, 5, 1}, {2, 10, 5, 2}};
    pitlane::CallOut fills[2]{};
    int64_t matched = 0;
    const int64_t px = pitlane::auction_clear(tie, 2, 10, 0, fills, 2, &matched);
    expect(matched == 5 && px == 10, "closer price");
}

static void check_own() {
    pitlane::OwnPair pair;
    pair.want_a = 1;
    pair.want_b = 2;
    pitlane::TopQuote a{};
    a.venue = 1;
    a.live = 1;
    a.bid_px = 105;
    a.ask_px = 106;
    a.bid_sz = 1;
    a.ask_sz = 1;
    pitlane::TopQuote b{};
    b.venue = 2;
    b.live = 1;
    b.bid_px = 100;
    b.ask_px = 101;
    b.bid_sz = 1;
    b.ask_sz = 1;
    pair.feed(a);
    pair.feed(b);
    expect(pair.ready() && pair.cross_ticks() == 4, "own cross");
    pair.clear();
    expect(!pair.ready(), "cleared");

    pitlane::SlotGate gate;
    gate.pair.want_a = 1;
    gate.pair.want_b = 2;
    expect(!gate.on_quote(a), "one side");
    expect(gate.on_quote(b), "both sides");
    gate.on_drop();
    expect(!gate.on_quote(a) && !gate.on_quote(b), "needs seed");
    gate.seeded(a, b);
    expect(gate.pair.ready() && gate.pair.cross_ticks() == 4, "seeded");
}

static void check_quote_ordered() {
    pitlane::QuoteFold<8> fold;
    const pitlane::FoldEdit edits[2] = {
        {pitlane::FoldOp::Set, 1, 100, 4},
        {pitlane::FoldOp::Set, 2, 101, 3},
    };
    pitlane::SeqWatch watch{};
    expect(fold.apply_ordered(1, edits, 2, watch), "ordered first");
    int64_t bp = 0, bq = 0, ap = 0, aq = 0;
    expect(fold.top(&bp, &bq, &ap, &aq) && bp == 100 && ap == 101, "top");
    expect(!fold.apply_ordered(3, edits, 2, watch), "gap fail");
    expect(fold.top(&bp, &bq, &ap, &aq) && bp == 100, "book kept");
    expect(fold.apply_ordered(2, edits, 2, watch), "next");
    fold.clear();
    expect(!fold.built(), "cleared");
}

static void check_cumtick() {
    pitlane::CumSnap prev{};
    prev.vol = 10;
    prev.turn = 1000;
    prev.last = 100;
    prev.bid1 = 99;
    prev.ask1 = 101;
    pitlane::CumSnap cur = prev;
    cur.vol = 11;
    cur.turn = 1100;
    cur.last = 100;
    int64_t px[4]{};
    int64_t lots[4]{};
    expect(pitlane::infer_lots(prev, cur, 1, 1, true, px, lots, 4) == 1 && px[0] == 100 && lots[0] == 1,
           "flat one lot");
    cur.vol = 12;
    cur.turn = 1200;
    expect(pitlane::infer_lots(prev, cur, 1, 1, false, px, lots, 4) >= 1, "split lots");
    expect(pitlane::infer_lots(prev, cur, 0, 1, false, px, lots, 4) == -1, "bad tick");
}

static void check_auction_est_maker() {
    uint8_t side = 0;
    expect(pitlane::auction_estimate_fill(7, 5, &side) == 5 && side == 1, "est buy");
    expect(pitlane::auction_estimate_fill(-3, 10, &side) == 3 && side == 2, "est sell");
    expect(pitlane::maker_only_style(pitlane::Style::Passive), "maker style");
    pitlane::Rung asks[1]{{100, 1}};
    const auto hit = pitlane::walk_buy(pitlane::Style::Passive, 100, 1, asks, 1);
    expect(hit.kind == pitlane::Arrival::Drop, "maker drop cross");
}

static pitlane::MapOpt map_opt_tmp() noexcept {
    pitlane::MapOpt opt{};
    opt.hugepage = 0;
    opt.lock = 0;
    opt.prefault = 1;
    return opt;
}

static int g_reports = 0;
static void on_rep(void*, const pitlane::Report&) { ++g_reports; }

static void check_framework() {
    char path[64];
    std::snprintf(path, sizeof path, "/tmp/pitlane-lane-%d.bin", static_cast<int>(::getpid()));
    ::unlink(path);
    pitlane::MapInfo map{};
    expect(pitlane::map_create(path, pitlane::desk_bytes(), &map, map_opt_tmp()) == 0, "lane map");
    pitlane::Desk* desk = pitlane::desk_view(&map, 1);
    expect(desk != nullptr, "lane desk");

    pitlane::VenueCfg cfg{};
    cfg.lot = 1;
    pitlane::PaperVenue venue(cfg);
    const pitlane::Rung asks[] = {{100, 5}};
    venue.set_levels(1, nullptr, 0, asks, 1);
    venue.set_band(1, 90, 110);

    pitlane::DrillBook drills;
    expect(drills.set(42, pitlane::DrillCue::FillAll, 100, 0), "drill set");
    expect(drills.set(43, pitlane::DrillCue::Closed), "drill closed");
    expect(static_cast<uint8_t>(pitlane::DrillCue::Closed) == 10, "closed=NotTrading");

    pitlane::TradeLane lane{};
    lane.venue = &venue;
    lane.drills = &drills;
    lane.intent_reader = 0;
    lane.report_mask = 0;

    pitlane::StratHook hook{};
    hook.on_report = on_rep;
    const pitlane::LoopHooks loop = pitlane::strat_bind_loop(hook);
    expect(loop.on_report == on_rep, "strat bind");

    const int64_t stamp = (9 * 60 + 40) * 60 * 1000000ll;
    pitlane::Intent in{};
    in.tag = 42;
    in.name = 1;
    in.acct = 1;
    in.px = 100;
    in.qty = 2;
    in.stamp_us = stamp;
    in.side = 1;
    in.act = static_cast<uint8_t>(pitlane::Act::Enter);
    in.style = 1;
    expect(pitlane::strat_stage_enter(*desk, in, 0), "stage enter");
    const int n = pitlane::lane_beat(lane, *desk, stamp);
    expect(n >= 1, "lane beat drill fill");

    pitlane::Report r{};
    bool fresh = false;
    uint64_t cur = 0;
    expect(desk->reports.at(cur, 0, r, &fresh) && fresh, "report present");
    expect(r.phase == static_cast<uint8_t>(pitlane::Phase::Filled) ||
               r.phase == static_cast<uint8_t>(pitlane::Phase::Partial) ||
               r.phase == static_cast<uint8_t>(pitlane::Phase::Working) ||
               r.phase == static_cast<uint8_t>(pitlane::Phase::Staged),
           "report phase");

    pitlane::FeedHub hub{};
    hub.desk = desk;
    pitlane::Quote q{};
    q.name = 1;
    q.bid_px = 99;
    q.ask_px = 101;
    expect(pitlane::feed_push_mark(hub, 1, pitlane::YardMark{}, &q), "feed quote desk");

    pitlane::map_release(&map);
    ::unlink(path);
}

int main() {
    check_steps();
    check_books();
    check_gap();
    check_venue();
    check_calendar();
    check_quote();
    check_framework();
    check_walk();
    check_bar();
    check_window();
    check_cover();
    check_target();
    check_session_book();
    check_own();
    check_quote_ordered();
    check_cumtick();
    check_auction_est_maker();
    if (g_fails) {
        std::fprintf(stderr, "%d book check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
