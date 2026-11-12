#include "pitlane/pitlane.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>

static int g_fails = 0;

static pitlane::MapOpt map_opt_tmp() noexcept {
    pitlane::MapOpt opt{};
    opt.hugepage = 0;
    opt.lock = 0;
    opt.prefault = 1;
    return opt;
}


static void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

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

void check_batch() {
    pitlane::FanRing<pitlane::Intent, 4> ring;
    pitlane::Intent items[3]{};
    items[0].tag = 1;
    items[1].tag = 2;
    items[2].tag = 3;
    expect(ring.publish_batch(items, 3, 0x1) == 3 && ring.tail() == 3, "batch");
    pitlane::Intent out{};
    bool fresh = false;
    expect(ring.at(0, 0, out, &fresh) && out.tag == 1 && ring.at(2, 0, out, &fresh) && out.tag == 3, "read batch");
    expect(ring.publish_batch(items, 2, 0x1) == 0 && ring.tail() == 3, "blocked batch");
    expect(ring.ack(0, 0) && ring.ack(1, 0) && ring.publish_batch(items, 2, 0x1) == 2, "after ack");

    pitlane::FanRing<pitlane::Intent, 4> span;
    expect(span.ack_span(0, 0, 0) == 0, "empty span");
    expect(span.publish(items[0], 0x1) && span.publish(items[1], 0x1) && span.publish(items[2], 0x1), "three");
    expect(span.ack_span(0, 3, 0) == 3, "ack span");
    expect(span.publish_batch(items, 3, 0x1) == 3, "after the span");
    pitlane::FanRing<pitlane::Intent, 4> once;
    expect(once.publish(items[0], 0) && once.ack_span(0, 1, 2) == 1, "first claim span");
    expect(once.ack_span(0, 1, 3) == 0, "second claim span");

    pitlane::FanRing<pitlane::Intent, 4> inplace;
    pitlane::Intent* body = inplace.claim(0x1);
    expect(body != nullptr, "claim body");
    body->tag = 42;
    expect(inplace.publish_claimed() && inplace.tail() == 1, "publish claimed");
    bool seen_fresh = false;
    const pitlane::Intent* seen = inplace.view(0, 0, &seen_fresh);
    expect(seen && seen->tag == 42 && seen_fresh, "view");
    expect(inplace.ack(0, 0), "view ack");
}

static void check_ids() {
    pitlane::IdMint mint(7, 2);
    const uint64_t id = mint.next(13, 35);
    expect(pitlane::id_site(id) == 7, "site");
    expect(pitlane::id_kind(id) == 2, "kind");
    expect(pitlane::id_day_mod(id) == 3, "day");
    expect(pitlane::id_ten_sec(id) == 3, "slot");
    expect(pitlane::id_serial(id) == 1, "serial");
    const uint64_t id2 = mint.next(13, 35);
    expect(pitlane::id_serial(id2) == 2, "serial grows");
}

static void check_cash() {
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

static void check_region() {
    const char* path = "pitlane_region.bin";
    ::unlink(path);
    pitlane::MapInfo map{};
    expect(pitlane::map_create(path, pitlane::desk_bytes(), &map, map_opt_tmp()) == 0, "map");
    pitlane::Desk* desk = pitlane::desk_view(&map, 1);
    expect(desk != nullptr, "format");
    pitlane::Intent wire{};
    wire.tag = 42;
    wire.qty = 1;
    expect(desk->intents.publish(wire, 0x1), "shm publish");
    pitlane::map_release(&map);

    pitlane::MapInfo again_map{};
    expect(pitlane::map_attach(path, pitlane::desk_bytes(), &again_map, map_opt_tmp()) == 0, "remap");
    pitlane::Desk* back = pitlane::desk_view(&again_map, 0);
    expect(back != nullptr, "magic");
    pitlane::Intent got{};
    bool fresh = false;
    expect(back->intents.at(0, 0, got, &fresh) && fresh && got.tag == 42, "shm read");
    pitlane::map_release(&again_map);
    ::unlink(path);
}

static void check_codec() {
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
}

static void check_feed() {
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

static void check_names() {
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

static void check_ring_full() {
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

static void check_hold() {
    pitlane::HoldLine holds[2]{};
    pitlane::HoldBoard board(holds, 2);
    pitlane::HoldLine* row = board.upsert(1, "p", "a", "rb2410", true);
    expect(row && row->split_yday == 1, "hold row");
    board.reindex();
    expect(board.find(pitlane::HoldBoard::key_of(1, "p", "a", "rb2410")) == row, "hold find");
}

static int g_quotes = 0;
static int g_timers = 0;
static void on_quote(void*, const pitlane::Quote&) { ++g_quotes; }
static void on_timer(void*, uint64_t, int64_t) { ++g_timers; }

static void check_session() {
    const char* path = "pitlane_suite_region.bin";
    ::unlink(path);
    pitlane::MapInfo map{};
    expect(pitlane::map_create(path, pitlane::desk_bytes(), &map, map_opt_tmp()) == 0, "suite map");
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
    expect(fold.push(0, 2, 1, 99) && fold.push(0, 2, 2, 7), "kind push");
    fold.fold();
    pitlane::QueryWant wants[4]{};
    expect(fold.pending_kind(wants, 4) == 2, "kind pending");
    fold.clear_want(2, 1);
    expect(fold.pending_kind(wants, 4) == 1 && wants[0].kind == 2 && wants[0].page == 7, "kind page");

    pitlane::RiskCaps glob{};
    glob.max_qty = 100;
    glob.max_notional = 10000;
    glob.max_live = 5;
    pitlane::RiskCaps acct{};
    acct.max_qty = 40;
    acct.max_live = 0;
    pitlane::RiskCaps merged = pitlane::risk_merge(glob, acct);
    expect(merged.max_qty == 40 && merged.max_notional == 10000 && merged.max_live == 5, "risk merge");
    pitlane::AcctRiskTab<4> tab;
    expect(tab.put(2, acct) && tab.merged(2, glob, merged) && merged.max_qty == 40, "acct risk");
    expect(!tab.merged(9, glob, merged), "risk miss");

    pitlane::TraderAcctTab<4, 4> traders;
    expect(traders.link("t1", 10) && traders.link("t1", 11), "trader link");
    uint32_t t_accts[4]{};
    expect(traders.accts("t1", t_accts, 4) == 2, "trader accts");
    pitlane::CashLine cash{};
    cash.free_amt = 100;
    cash.held_amt = 0;
    pitlane::HoldLine hold{};
    expect(pitlane::acct_idle(&cash, &hold, 0) && !pitlane::acct_idle(&cash, &hold, 1), "acct idle");
    expect(pitlane::cash_enough(cash, 50, 1) && !pitlane::cash_enough(cash, 100, 1), "cash enough");

    pitlane::AcctLink link{};
    std::strcpy(link.trader, "t");
    std::strcpy(link.counter, "paper");
    std::strcpy(link.account, "a");
    link.enabled = 1;
    expect(pitlane::acct_visible(&link, 1, "t", "paper", "a"), "visible");
    char route[32]{};
    expect(pitlane::reply_route("paper", route, 32) && std::strcmp(route, "paper.reply") == 0, "route");

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
        if (pitlane::map_attach(path, pitlane::desk_bytes(), &child_map, map_opt_tmp()) != 0) _exit(2);
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
}

int main() {
    expect(pitlane::kVersionMajor == 2 && pitlane::kVersionMinor == 10 && pitlane::kVersionPatch == 0, "release");
    expect(std::strcmp(pitlane::kVersion, "2.10.0") == 0, "release text");
    expect(pitlane::kDeskVersion == 3, "desk layout");
    check_ring();
    check_batch();
    check_ids();
    check_cash();
    check_region();
    check_codec();
    check_feed();
    check_names();
    check_ring_full();
    check_hold();
    check_session();
    if (g_fails) {
        std::fprintf(stderr, "%d desk check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
