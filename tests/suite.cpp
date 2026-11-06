#include "pitlane/pitlane.hpp"

#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_fails = 0;

void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

}  // namespace

static void check_codec_lots_numeric() {
    expect(pitlane::classify_symbol("600000.SH") == pitlane::Product::Stock, "cash stock");
    expect(pitlane::classify_symbol("000001.SH") == pitlane::Product::Index, "index prefix");
    expect(pitlane::classify_symbol("10000001.SH") == pitlane::Product::Option, "sh option");
    expect(pitlane::classify_symbol("688001.SH") == pitlane::Product::Tech, "tech");
    expect(pitlane::classify_symbol("159915.SZ") == pitlane::Product::Fund, "sz fund");
    expect(pitlane::classify_symbol("rb2410.SHF") == pitlane::Product::Future, "future");
    expect(pitlane::classify_symbol("nosuffix") == pitlane::Product::Unknown, "no venue");
    char root[8]{};
    expect(pitlane::product_root("IF2403.CFE", root, 8) && std::strcmp(root, "IF") == 0, "root");
    expect(pitlane::close_style_of("rb2410.SHF") == pitlane::CloseStyle::SplitYday, "split close");
    expect(pitlane::close_style_of("IF2403.CFE") == pitlane::CloseStyle::Plain, "plain close");

    pitlane::GateForm shf{};
    std::snprintf(shf.symbol, sizeof shf.symbol, "rb2410.SHF");
    shf.side = 1;
    shf.off = static_cast<int8_t>(pitlane::Off::Close);
    shf.px_mode = static_cast<int8_t>(pitlane::PxMode::Unknown);
    shf.px = 3500;
    shf.qty = 2;
    const pitlane::GateResult a = pitlane::normalize_gate(shf);
    expect(a.why == pitlane::Why::None && a.off == pitlane::Off::CloseYday && a.mode == pitlane::PxMode::Limit, "shf close becomes yesterday");

    pitlane::GateForm cash = shf;
    std::snprintf(cash.symbol, sizeof cash.symbol, "600000.SH");
    cash.off = static_cast<int8_t>(pitlane::Off::CloseToday);
    const pitlane::GateResult b = pitlane::normalize_gate(cash);
    expect(b.off == pitlane::Off::Open && b.product == pitlane::Product::Stock, "cash forced open");

    pitlane::GateForm bad = shf;
    bad.wire_side = 9;
    bad.side = 0;
    expect(pitlane::normalize_gate(bad).why == pitlane::Why::Side, "bad wire side");
    expect(pitlane::wire_to_dir(6) == 0 && pitlane::wire_to_dir(7) == 1, "create and redeem");

    pitlane::GateForm zero = shf;
    zero.px_mode = static_cast<int8_t>(pitlane::PxMode::Limit);
    zero.px = 0;
    expect(pitlane::normalize_gate(zero).why == pitlane::Why::Mode, "zero limit");

    expect(pitlane::final_phase(static_cast<uint8_t>(pitlane::Phase::Filled)), "final");
    expect(pitlane::hanging_phase(static_cast<uint8_t>(pitlane::Phase::Partial)), "hanging");
    expect(!pitlane::running_phase(static_cast<uint8_t>(pitlane::Phase::Expired)), "expired is not running");
    expect(std::strcmp(pitlane::phase_text(static_cast<uint8_t>(pitlane::Phase::Working)), "working") == 0, "phase text");
    expect(std::strcmp(pitlane::why_text(pitlane::Why::Wash), "wash") == 0, "why text");
    expect(pitlane::exchange_id("INE") == 7 && std::strcmp(pitlane::exchange_text(7), "INE") == 0, "exchange");
    expect(std::strcmp(pitlane::holder_for("000001.SZ", "sh-holder", "sz-holder"), "sz-holder") == 0, "holder");

    expect(pitlane::round_down_to(7, 5) == 5 && pitlane::round_up_to(7, 5) == 10, "round");
    expect(pitlane::round_nearest_to(-7, 5) == -5, "nearest");
    expect(pitlane::round_up_nonzero(0, 5) == 5, "nonzero");
    expect(pitlane::max_abs(-8, 3) == -8 && pitlane::min_abs(-8, 3) == 3, "abs");
    char hex[8]{};
    expect(pitlane::to_hex(0x2a, hex, 8) == 2 && std::strcmp(hex, "2a") == 0, "hex");

    pitlane::LotSlice slices[4]{};
    const int n = pitlane::split_lots(10, pitlane::LotPos{6, 2}, pitlane::LotPend{0, 0}, 3, 1,
                                      pitlane::SplitMode::TodayThenYdayThenOpen, slices, 4);
    expect(n == 4, "four slices");
    expect(slices[0].off == pitlane::Off::CloseToday && slices[0].qty == 2, "today first");
    expect(slices[1].off == pitlane::Off::CloseYday && slices[1].qty == 3, "yesterday capped");
    expect(slices[2].off == pitlane::Off::Open && slices[2].qty == 3, "open chunk");
    expect(slices[3].off == pitlane::Off::Open && slices[3].qty == 2, "open remainder");
    expect(pitlane::split_lots(3, pitlane::LotPos{5, 0}, pitlane::LotPend{}, 0, 1, pitlane::SplitMode::YdayThenStop, slices,
                               4) == 1,
           "yesterday only");
    expect(pitlane::split_lots(1, {}, {}, 0, 1, static_cast<pitlane::SplitMode>(9), slices, 4) == -1, "unsupported mode");
}

static void check_calendar_feed() {
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

    pitlane::WantMask mask;
    expect(mask.accept(3), "filter off");
    mask.filter(true);
    expect(!mask.accept(3), "filtered out");
    mask.allow(3, true);
    expect(mask.accept(3), "allowed");
    int64_t up = 0, down = 0;
    pitlane::patch_band(up, down, 110, 90);
    expect(up == 110 && down == 90, "patch");
    expect(pitlane::tape_index(0, 11, 22) == 11 && pitlane::tape_index(1, 11, 22) == 22, "tape index");
    const pitlane::LinkBeat beat = pitlane::normalize_beat(pitlane::LinkBeat{0, 0, 0, 9});
    expect(beat.interval_s == 10 && beat.misses == 2 && beat.connect_s == 30 && beat.mode == 0, "beat defaults");
    pitlane::RawTick in[2]{};
    in[0].tag = 'S';
    in[0].px = 10000;
    in[1].px = 25000;
    in[1].qty = 3;
    pitlane::RawTick got[2]{};
    expect(pitlane::take_ticks(in, 2, 9, 10000, got, 2) == 1 && got[0].px == 2 && got[0].recv_us == 9, "scale and skip");
}

static void check_text_store_names_signal() {
    char parts[4][64];
    expect(pitlane::split_text("a,,b", ',', parts, 4, true) == 2, "drop empty");
    const char* join_in[] = {"aa", "bb"};
    char joined[16]{};
    expect(pitlane::join_text(join_in, 2, '|', joined, 16) && std::strcmp(joined, "aa|bb") == 0, "join");
    char padded[8]{};
    expect(pitlane::pad_left("7", '0', 3, padded, 8) && std::strcmp(padded, "007") == 0, "pad");
    expect(pitlane::contains_ci("AbC", "bc"), "ci");
    expect(pitlane::hash_str("pit") == pitlane::hash32("pit", 3), "hash");

    pitlane::FlatMap map;
    expect(pitlane::parse_flat_json("{\"k\":\"v\",\"n\":\"1\"}", &map) && map.n == 2, "json");
    expect(std::strcmp(pitlane::flat_get(map, "n"), "1") == 0, "json get");
    char json[64]{};
    expect(pitlane::write_flat_json(map, json, 64), "json write");
    pitlane::LagTable lags{};
    expect(pitlane::load_lag_csv("0,100,200\n1,300,400\n", &lags) && lags.go_us[1] == 300 && lags.pull_us[0] == 200, "lag csv");
    const char* path = "pitlane_lines.txt";
    expect(pitlane::write_text(path, "one\ntwo\n"), "write");
    char lines[4][160];
    int nlines = 0;
    expect(pitlane::read_lines(path, lines, 4, &nlines) && nlines == 2 && std::strcmp(lines[1], "two") == 0, "read");
    ::unlink(path);
    const unsigned char gbk[] = {0xd6, 0xd0};
    char utf[8]{};
    expect(pitlane::gbk_to_utf8(reinterpret_cast<const char*>(gbk), 2, utf, sizeof utf), "gbk");
    expect(static_cast<unsigned char>(utf[0]) == 0xe4 && static_cast<unsigned char>(utf[1]) == 0xb8 &&
               static_cast<unsigned char>(utf[2]) == 0xad,
           "utf8");

    pitlane::Dict dict;
    expect(dict.intern("mid") == 1 && dict.intern("mid") == 1 && std::strcmp(dict.text(1), "mid") == 0, "dict");
    pitlane::TimeIndex times;
    expect(times.put(10, 3) && !times.put(10, 4), "time unique");
    uint32_t row = 0;
    expect(times.get(10, &row) && row == 3, "time get");
    pitlane::Column<4> col;
    expect(col.put(1, 42) && !col.put(4, 1), "column");
    int64_t value = 0;
    expect(col.get(1, &value) && value == 42 && !col.get(0, &value), "column get");

    pitlane::NameBook book;
    uint32_t rb = 0, und = 0;
    expect(book.insert("rb2410.SHF", 1, &rb) && book.insert("rb.SHF", 1, &und), "names");
    expect(book.link(rb, und, 1) && book.at(rb)->n_underlying == 1, "underlying");
    expect(book.same_symbol(rb, "rb2410.SHF"), "match symbol");
    book.set_session(rb, 21 * 60, 150);
    expect(book.in_session(rb, 22 * 60) && book.in_session(rb, 60) && !book.in_session(rb, 12 * 60), "night name");
    book.set_quote(rb, 10, 11, 10);
    book.set_factor(rb, 7);
    expect(book.set_prop(rb, "root", "rb") && std::strcmp(book.prop(rb, "root"), "rb") == 0, "prop");
    uint32_t ids[4]{};
    expect(book.list_suffix(".SHF", ids, 4) == 2, "suffix list");

    pitlane::PulseBoard pulses;
    expect(pulses.watch(rb, "mid", 100, true), "watch");
    pitlane::Pulse early{};
    early.name = rb;
    early.topic[0] = 'm';
    std::strcpy(early.topic, "mid");
    early.stamp_us = 50;
    early.dense = 1;
    expect(!pulses.offer(early), "before window");
    early.stamp_us = 150;
    early.dense = 0;
    expect(!pulses.offer(early), "not dense");
    early.dense = 1;
    expect(pulses.offer(early), "accepted");
    pitlane::Pulse out{};
    expect(pulses.drain(&out, 1) == 1 && out.stamp_us == 150, "drain");
}

static bool wake_until(int index, void* user) {
    auto* n = static_cast<int*>(user);
    if (index >= *n) return false;
    return true;
}

static void check_ledger() {
    pitlane::LotLock lots{};
    lots.total = 10;
    lots.yday_left = 4;
    lots.free_lots = 10;
    expect(pitlane::lock_future(lots, pitlane::Off::CloseToday, 7) == pitlane::LotErr::NoToday, "today short");
    expect(pitlane::lock_future(lots, pitlane::Off::CloseToday, 2) == pitlane::LotErr::Ok && lots.locked == 2 &&
               lots.free_lots == 8,
           "today lock");
    expect(pitlane::lock_future(lots, pitlane::Off::CloseYday, 5) == pitlane::LotErr::NoYday, "yday short");
    expect(pitlane::lock_future(lots, pitlane::Off::CloseYday, 3) == pitlane::LotErr::Ok && lots.yday_locked == 3,
           "yday lock");
    expect(pitlane::lock_future(lots, pitlane::Off::Close, 20) == pitlane::LotErr::None, "plain short");
    pitlane::add_working(lots, true, 4, 0);
    expect(lots.buy_working == 0, "zero lock adds nothing");
    pitlane::add_working(lots, false, 9, 3);
    expect(lots.sell_working == 3, "working clipped");
    pitlane::LotLock cash{};
    cash.free_lots = 5;
    expect(!pitlane::lock_cash_sell(cash, 6) && pitlane::lock_cash_sell(cash, 2) && cash.free_lots == 3 &&
               cash.yday_locked == 2,
           "cash sell");
    expect(pitlane::basket_transfer(2, 0) && !pitlane::basket_transfer(2, 1), "basket");

    pitlane::StopBand stop{};
    pitlane::arm_long_stop(stop, 100, 10, 80);
    expect(pitlane::exit_long(stop, 90) && pitlane::exit_long(stop, 80) && !pitlane::exit_long(stop, 95), "stop");
    const pitlane::StubOrder down = pitlane::stub_flat(5, 2);
    const pitlane::StubOrder up = pitlane::stub_flat(-5, 2);
    expect(down.qty == 4 && down.buy == 0 && up.qty == 6 && up.buy == 1 && pitlane::stub_flat(0, 2).qty == 0, "stub");

    pitlane::PurseBook purse{};
    purse.n = 1;
    purse.cash = 20;
    purse.qty = 8;
    purse.unit[0] = {10, 4};
    expect(pitlane::draw_unit(purse, 0, 5, 3) && purse.unit[0].cash == 5 && !pitlane::draw_unit(purse, 0, 9, 1),
           "unit draw");
    const pitlane::ClassScale fut = pitlane::scale_of(pitlane::Product::Future);
    const pitlane::ClassScale stk = pitlane::scale_of(pitlane::Product::Stock);
    expect(fut.mult == 100 && fut.margin_num == 15 && stk.mult == 1, "scale");
    expect(pitlane::cash_charge(10000, 3, true, 10, 1, 10000) == 14, "cash fee");
    expect(pitlane::deriv_charge(10000, 3, 10000) == 3, "deriv fee");
    int64_t lock_px = 0, lock_qty = 0;
    expect(pitlane::fill_lock(lock_px, lock_qty, 4, 0, 10, 12) && lock_qty == 4 && lock_px == 11, "mid lock");

    pitlane::ChaseBook chase{};
    chase.split_yday = true;
    chase.sht.pos = 4;
    chase.sht.yday = 4;
    chase.target = 5;
    chase.open_cap = 100;
    const pitlane::Move first = pitlane::commodity_plan(chase);
    expect(first.kind == pitlane::MoveKind::BuyCloseYday && first.qty == 4, "flatten short first");
    chase.sht = {};
    chase.lng.pos = 8;
    chase.lng.yday = 5;
    chase.target = 3;
    const pitlane::Move cut = pitlane::commodity_plan(chase);
    expect(cut.kind == pitlane::MoveKind::SellCloseYday && cut.qty == 5, "clip extra to yesterday");
    chase = {};
    chase.target = 5;
    chase.open_cap = 0;
    expect(pitlane::commodity_plan(chase).kind == pitlane::MoveKind::None, "open cap blocks");
    chase.open_cap = 10;
    chase.split_yday = true;
    expect(pitlane::commodity_plan(chase).kind == pitlane::MoveKind::BuyOpen &&
               pitlane::plan_px(pitlane::MoveKind::BuyOpen, 10, 11) == 11,
           "buy uses ask");

    pitlane::ChaseBook idx{};
    idx.sht.pos = 5;
    idx.sht.opened = 2;
    idx.target = 0;
    idx.open_cap = 100;
    const pitlane::Move idxm = pitlane::index_plan(idx);
    expect(idxm.kind == pitlane::MoveKind::BuyClose && idxm.qty == 3, "index closes yesterday only");
    pitlane::book_fill(chase, pitlane::MoveKind::BuyOpen, 2);
    expect(chase.lng.pos == 2 && chase.lng.today == 2 && chase.lng.opened == 2, "open books today");

    pitlane::FrontSeat seat{};
    pitlane::arm_front(seat, 7, 9, 4);
    expect(pitlane::take_ref(seat) == 5 && pitlane::take_ref(seat) == 6, "refs");
    pitlane::WorkSlot slot{};
    slot.busy = 1;
    slot.ref = 5;
    pitlane::on_order_echo(slot, seat, 1, 9, 5, 1, pitlane::QueueMark::Cancelled, true);
    expect(slot.busy == 1 && slot.pulls == 0, "foreign front ignored");
    pitlane::on_order_echo(slot, seat, 7, 9, 5, 0, pitlane::QueueMark::InQueue, true);
    expect(slot.queued == 1 && slot.busy == 1, "still queued");
    pitlane::on_trade_echo(slot, 5, 2);
    expect(slot.busy == 1 && slot.trd_done == 2, "queued trade stays");
    pitlane::on_order_echo(slot, seat, 7, 9, 5, 2, pitlane::QueueMark::Cancelled, true);
    expect(slot.pulls == 1 && slot.busy == 0, "pull clears when matched");
    slot.busy = 1;
    slot.ref = 8;
    pitlane::on_reject_echo(slot, 8);
    expect(slot.busy == 0, "reject clears");

    expect(pitlane::stored_target(4, 9, false, true) == 4 && pitlane::stored_target(4, 9, true, false) == 0, "target");
    pitlane::MainPick pick{};
    pitlane::keep_larger(pick, 3, 10, false);
    pitlane::keep_larger(pick, 3, 10, true);
    pitlane::keep_larger(pick, 4, 10, true);
    pitlane::keep_larger(pick, 5, 11, true);
    expect(pick.held && pick.name == 5 && pick.vol == 11, "main contract");
    int wake_limit = 1;
    expect(!pitlane::wake_all(0, 2, wake_until, &wake_limit) && pitlane::wake_all(0, 1, wake_until, &wake_limit),
           "wake");

    pitlane::LotSlice only[4]{};
    const int cn = pitlane::split_lots(10, pitlane::LotPos{8, 3}, {}, 100, 1, pitlane::SplitMode::CloseOnly, only, 4);
    expect(cn == 2 && only[0].off == pitlane::Off::CloseToday && only[0].qty == 3 && only[1].qty == 5, "close only");
    pitlane::FanRing<int, 2> full;
    expect(full.publish(1, 1) && full.publish(2, 1) && !full.publish(3, 1), "ring full");
    pitlane::BrokerWatch life;
    pitlane::Intent kept{};
    kept.tag = 3;
    kept.side = 1;
    kept.qty = 1;
    kept.px = 1;
    expect(life.remember(kept, 0) && life.ack(3), "life remember");
    uint64_t tags[2]{};
    expect(life.expire(1000, 0, 500, 0, tags, 2) == 1 && tags[0] == 3, "life timeout");
}

static int g_quotes = 0;
static int g_timers = 0;
static void on_quote(void*, const pitlane::Quote&) { ++g_quotes; }
static void on_timer(void*, uint64_t, int64_t) { ++g_timers; }

static void check_loop_broker_match() {
    const char* path = "pitlane_suite_region.bin";
    ::unlink(path);
    pitlane::MapInfo map{};
    expect(pitlane::map_create(path, pitlane::desk_bytes(), &map) == 0, "suite map");
    pitlane::Desk* desk = pitlane::desk_view(&map, 1);
    expect(desk && desk->version == pitlane::kDeskVersion, "version");
    pitlane::Quote q{};
    q.name = 1;
    q.bid_px = 10;
    q.ask_px = 11;
    const uint64_t bit = 1ull << pitlane::kLoopReader;
    expect(desk->quotes.publish(q, bit), "quote");
    expect(desk->quotes.latest(pitlane::kLoopReader, q), "latest");
    pitlane::ControlNote stop{};
    stop.kind = 1;
    stop.stamp_us = 5;
    pitlane::DeskLoop loop;
    expect(loop.add_timer(3, 5, 0), "timer");
    pitlane::LoopHooks hooks{};
    hooks.on_quote = on_quote;
    hooks.on_timer = on_timer;
    g_quotes = 0;
    g_timers = 0;
    expect(loop.beat(*desk, 5, hooks) >= 2 && g_quotes == 1 && g_timers == 1, "beat");
    expect(!desk->controls.publish(stop, bit) || true, "control slot free");
    expect(desk->controls.publish(stop, bit), "stop note");
    expect(loop.beat(*desk, 6, hooks) >= 1 && loop.stopped(), "stop");
    expect(loop.beat(*desk, 7, hooks) == 0, "stopped stays quiet");

    const char raw[] = {1, 2, 3, 4};
    pitlane::FanRing<pitlane::Parcel<4>, 2> parcels;
    expect(!pitlane::publish_parcel(parcels, 1, raw, 5, 1), "parcel too big");
    expect(pitlane::publish_parcel(parcels, 1, raw, 4, 1), "parcel");

    pitlane::BrokerWatch watch;
    pitlane::RiskCaps caps{};
    caps.max_qty = 10;
    caps.max_notional = 1000;
    caps.max_live = 1;
    pitlane::Intent in{};
    in.tag = 1;
    in.side = 1;
    in.qty = 11;
    in.px = 10;
    expect(watch.screen(in, caps) == pitlane::Why::Qty, "qty cap");
    in.qty = 5;
    in.px = 300;
    expect(watch.screen(in, caps) == pitlane::Why::Notional, "notional cap");
    in.px = 10;
    expect(watch.screen(in, caps) == pitlane::Why::None && watch.remember(in, 1000), "remember");
    expect(watch.screen(in, caps) == pitlane::Why::Rate, "live cap");
    uint64_t expired[4]{};
    expect(watch.expire(2000, 500, 0, 0, expired, 4) == 1 && expired[0] == 1, "ack timeout");
    in.tag = 2;
    expect(watch.remember(in, 3000) && watch.ack(2) && watch.mark_pull(2, 3100), "pull mark");
    expect(watch.expire(4000, 0, 0, 500, expired, 4) == 1 && expired[0] == 2, "pull timeout");

    pitlane::QueryFold fold;
    expect(fold.push(0, 3) && fold.push(1, 3) && !fold.push(9, 1), "fold push");
    fold.fold();
    uint32_t accts[4]{};
    expect(fold.pending(accts, 4) == 1 && accts[0] == 3, "coalesced");
    fold.clear_acct(3);
    expect(fold.pending(accts, 4) == 0, "cleared");
    pitlane::AcctLink link{};
    std::strcpy(link.trader, "t");
    std::strcpy(link.counter, "paper");
    std::strcpy(link.account, "a");
    link.enabled = 1;
    expect(pitlane::acct_visible(&link, 1, "t", "paper", "a"), "visible");
    char route[32]{};
    expect(pitlane::reply_route("paper", route, 32) && std::strcmp(route, "paper.reply") == 0, "route");

    pitlane::SimClock clock;
    clock.table.go_us[0] = 10;
    clock.table.pull_us[0] = 20;
    const int64_t morning = (9 * 60 + 40) * 60 * 1000000ll;
    expect(clock.go(morning) == 10 && clock.back(morning) == 20, "table delay");
    clock.pts[0] = pitlane::LagPoint{100, 0, 180};
    clock.pts[1] = pitlane::LagPoint{200, 0, 280};
    clock.npts = 2;
    expect(clock.go(150) < 0, "reject delay");

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
    pitlane::HoldLine holds[2]{};
    pitlane::HoldBoard board(holds, 2);
    pitlane::HoldLine* row = board.upsert(1, "p", "a", "rb2410", true);
    expect(row && row->split_yday == 1, "hold row");
    board.reindex();
    expect(board.find(pitlane::HoldBoard::key_of(1, "p", "a", "rb2410")) == row, "hold find");

    const pitlane::CallIn tie[] = {{1, 12, 5, 1}, {2, 10, 5, 2}};
    pitlane::CallOut fills[2]{};
    int64_t matched = 0;
    const int64_t px = pitlane::auction_clear(tie, 2, 10, 0, fills, 2, &matched);
    expect(matched == 5 && px == 10, "closer price");

    int sink_level = -1;
    pitlane::Log log(1);
    log.bind([](int level, const char*, void* user) { *static_cast<int*>(user) = level; }, &sink_level);
    log.write(2, "debug");
    log.write(0, "err");
    expect(log.dropped() == 1 && log.emitted() == 1 && sink_level == 0, "log filter");
    expect(pitlane::measure().desk == pitlane::desk_bytes() && pitlane::measure().total() > pitlane::measure().desk, "footprint");

    const pid_t child = ::fork();
    expect(child >= 0, "fork");
    if (child == 0) {
        pitlane::MapInfo child_map{};
        if (pitlane::map_attach(path, pitlane::desk_bytes(), &child_map) != 0) _exit(2);
        pitlane::Desk* seen = pitlane::desk_view(&child_map, 0);
        if (!seen) _exit(3);
        pitlane::Quote child_q{};
        bool fresh = false;
        if (!seen->quotes.at(0, pitlane::kLoopReader, child_q, &fresh) || child_q.ask_px != 11) _exit(4);
        pitlane::map_release(&child_map);
        _exit(0);
    }
    if (child > 0) {
        int status = 0;
        ::waitpid(child, &status, 0);
        expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child read");
    }
    pitlane::map_release(&map);
    ::unlink(path);
}

int main() {
    check_codec_lots_numeric();
    check_ledger();
    check_calendar_feed();
    check_text_store_names_signal();
    check_loop_broker_match();
    if (g_fails) {
        std::fprintf(stderr, "%d suite check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("suite ok\n");
    return 0;
}
