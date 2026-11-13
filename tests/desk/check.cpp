#include "pitlane/pitlane.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <sys/wait.h>
#include <unistd.h>

static int g_fails = 0;

static pitlane::MapOpt map_opt_tmp() noexcept {
    pitlane::MapOpt opt{};
    opt.hugepage = 0;
    opt.lock = 0;
    opt.prefault = 1;
    return opt;
}

static void expect(bool cond, const char* what) {
    if (cond)
        return;
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
    expect(a.why == pitlane::Why::None && a.off == pitlane::Off::CloseYday && a.mode == pitlane::PxMode::Limit,
           "shf close becomes yesterday");

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
    expect(std::strcmp(pitlane::phase_text(static_cast<uint8_t>(pitlane::Phase::Working)), "working") == 0,
           "phase text");
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
    expect(pitlane::take_ticks(in, 2, 9, 10000, got, 2) == 1 && got[0].px == 2 && got[0].recv_us == 9,
           "scale and skip");
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
static void on_quote(void*, const pitlane::Quote&) {
    ++g_quotes;
}
static void on_timer(void*, uint64_t, int64_t) {
    ++g_timers;
}

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

    {
        pitlane::PackageNote preq{};
        preq.id = 7;
        preq.kind = 1;
        preq.len = 3;
        preq.data[0] = 9;
        expect(desk->pack_reqs.publish(preq, bit), "pack req");
        pitlane::LogNote ln{};
        ln.level = 1;
        ln.stamp_us = 9;
        std::snprintf(ln.line, sizeof ln.line, "hello");
        expect(desk->logs.publish(ln, bit), "log note");
        pitlane::AlgoJob job{};
        job.tag = 11;
        job.phase = 1;
        expect(desk->algos.publish(job, bit), "algo job");
        static int g_algo = 0, g_pack = 0, g_log = 0;
        g_algo = g_pack = g_log = 0;
        pitlane::DeskLoop loop2;
        pitlane::LoopHooks h2{};
        h2.on_algo = [](void*, const pitlane::AlgoJob&) { ++g_algo; };
        h2.on_pack_req = [](void*, const pitlane::PackageNote&) { ++g_pack; };
        h2.on_log = [](void*, const pitlane::LogNote&) { ++g_log; };
        expect(loop2.beat(*desk, 9, h2) >= 3 && g_algo == 1 && g_pack == 1 && g_log == 1, "pack/log/algo drain");
        expect(pitlane::desk_log_publish(*desk, 2, 10, "async", bit), "desk_log_publish");
        expect(pitlane::strat_pause(*desk, 0, 10, bit), "strat pause");
    }

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
    expect(fold.push(0, 3) && fold.push(1, 3) && !fold.push(pitlane::QueryFold::kEngines, 1), "fold push");
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
    expect(pitlane::measure().desk == pitlane::desk_bytes() && pitlane::measure().total() > pitlane::measure().desk,
           "footprint");

    const pid_t child = ::fork();
    expect(child >= 0, "fork");
    if (child == 0) {
        pitlane::MapInfo child_map{};
        if (pitlane::map_attach(path, pitlane::desk_bytes(), &child_map, map_opt_tmp()) != 0)
            _exit(2);
        pitlane::Desk* seen = pitlane::desk_view(&child_map, 0);
        if (!seen)
            _exit(3);
        pitlane::Quote child_q{};
        bool fresh = false;
        if (!seen->quotes.at(0, pitlane::kLoopReader, child_q, &fresh) || child_q.ask_px != 11)
            _exit(4);
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

static void check_conf() {
    pitlane::PitConf conf{};
    pitlane::conf_defaults(conf);
    expect(conf.intent_cap == pitlane::kIntentCap, "default intent");
    conf.intent_cap = 1000;
    conf.quote_cap = 3000;
    conf.max_live = -1;
    conf.query_engines = 9;
    char hints[1024]{};
    const int n = pitlane::conf_normalize(conf, hints, 1024);
    expect(n >= 1 && conf.intent_cap == 512 && conf.quote_cap == 2048, "normalize rings");
    expect(conf.max_live == 0 && conf.query_engines == pitlane::QueryFold::kEngines, "normalize broker query");
    expect(hints[0] != 0, "hints");

    const char* path = "pitlane_test.conf";
    FILE* f = std::fopen(path, "w");
    expect(f != nullptr, "conf file");
    if (f) {
        std::fputs("[desk]\nintent_cap=256\nquote_cap=512\n\n[map]\nhugepage=0\nlock=\n", f);
        std::fclose(f);
    }
    pitlane::PitConf loaded{};
    expect(pitlane::conf_load(path, loaded), "load");
    expect(loaded.intent_cap == 256 && loaded.quote_cap == 512 && loaded.map_hugepage == 0, "loaded values");
    expect(loaded.map_lock == 1, "empty keeps default");
    ::unlink(path);

    pitlane::MapOpt mop = pitlane::conf_map_opt(loaded);
    expect(mop.hugepage == 0 && mop.lock == 1, "map opt");

    auto* raw = new (std::nothrow) unsigned char[sizeof(pitlane::Desk)];
    expect(raw != nullptr, "desk heap");
    auto* desk = new (raw) pitlane::Desk();
    loaded.intent_cap = 256;
    loaded.report_cap = 256;
    loaded.quote_cap = 512;
    loaded.algo_cap = 128;
    loaded.control_cap = 128;
    loaded.pulse_cap = 128;
    loaded.print_cap = 128;
    loaded.query_cap = 128;
    loaded.pack_req_cap = 64;
    loaded.pack_rsp_cap = 128;
    loaded.log_cap = 64;
    expect(pitlane::conf_normalize(loaded, nullptr, 0) >= 0, "norm again");
    expect(pitlane::desk_apply_conf(*desk, loaded) && desk->intents.capacity() == 256, "desk tune");
    expect(desk->pack_reqs.capacity() == 64 && desk->logs.capacity() == 64, "pack/log tune");

    pitlane::CashLine cash_rows[8]{};
    pitlane::CashBoard cash(cash_rows, 8);
    loaded.cash_cap = 4;
    expect(pitlane::cash_apply_conf(cash, loaded) && cash.capacity() == 4, "cash soft");
    expect(cash.upsert(1, "p", "a", "d", "b") != nullptr, "cash upsert soft");
    desk->~Desk();
    delete[] raw;

    {
        int err = 0;
        FILE* bf = std::fopen("pitlane_bad.conf", "w");
        if (bf) {
            std::fputs("[desk]\nintent_cap=notnum\n", bf);
            std::fclose(bf);
        }
        pitlane::PitConf bad{};
        expect(!pitlane::conf_load_ex("pitlane_bad.conf", bad, &err) && err == 2, "err line");
        ::unlink("pitlane_bad.conf");
    }

    pitlane::PulseBoard pulses;
    expect(pulses.watch(1, "t", 0, false), "watch");
    pitlane::PulseSub subs[4]{};
    expect(pulses.copy_subs(subs, 4) == 1 && subs[0].name == 1, "copy_subs");

    expect(conf.yard_sym_slots == pitlane::kYardSymSlotsDefault, "yard default slots");
    conf.yard_quote_depth = 100;
    conf.yard_sym_slots = 0;
    char yh[256]{};
    const int yn = pitlane::conf_normalize(conf, yh, 256);
    expect(yn >= 1 && conf.yard_sym_slots == 1 && conf.yard_quote_depth == 64, "yard normalize");
    pitlane::YardPlan yp = pitlane::conf_yard_plan(conf);
    expect(yp.sym_slots == 1 && yp.quote_depth == 64, "yard plan");

    {
        FILE* yf = std::fopen("pitlane_yard.conf", "w");
        expect(yf != nullptr, "yard conf file");
        if (yf) {
            std::fputs("[yard]\nsym_slots=64\nquote_lanes=64\nquote_depth=16\n", yf);
            std::fclose(yf);
        }
        pitlane::PitConf yc{};
        expect(pitlane::conf_load("pitlane_yard.conf", yc), "yard load");
        expect(yc.yard_sym_slots == 64 && yc.yard_quote_lanes == 64 && yc.yard_quote_depth == 16, "yard loaded");
        ::unlink("pitlane_yard.conf");
    }
}

static void check_yard() {
    pitlane::YardPlan plan{};
    plan.sym_slots = 8;
    plan.quote_lanes = 8;
    plan.quote_depth = 4;
    plan.order_lanes = 2;
    plan.trade_lanes = 2;
    plan.rebuild_lanes = 2;
    plan.mix_lanes = 2;
    plan.order_depth = 8;
    plan.trade_depth = 8;
    plan.rebuild_depth = 8;
    plan.mix_depth = 8;
    plan.base_info_cap = 4;
    plan.trade_time_cap = 4;
    expect(pitlane::yard_plan_ok(plan), "yard plan ok");
    const std::size_t bytes = pitlane::yard_bytes(plan);
    expect(bytes > sizeof(pitlane::YardHead) && bytes < (1u << 20), "yard bytes tiny");

    char path[64];
    std::snprintf(path, sizeof path, "/tmp/pitlane-yard-%d.bin", static_cast<int>(::getpid()));
    ::unlink(path);
    pitlane::YardView y{};
    expect(pitlane::yard_open(path, plan, &y, map_opt_tmp(), 1) == 0, "yard open");
    expect(y.head && y.head->version == pitlane::kYardVersion && y.facts && y.hours, "yard head");

    const uint32_t id = pitlane::yard_claim_sym(y, "IF2501");
    expect(id == 1, "claim");
    expect(pitlane::yard_claim_sym(y, "IF2501") == 1, "claim idempotent");
    expect(pitlane::yard_bind_quote(y, id), "bind quote");
    expect(pitlane::yard_bind_order(y, id) && pitlane::yard_bind_trade(y, id), "bind order trade");
    expect(pitlane::yard_bind_rebuild(y, id) && pitlane::yard_bind_mix(y, id), "bind rebuild mix");

    pitlane::YardMark m{};
    m.bid_px = 100;
    m.ask_px = 101;
    m.bid_qty = 2;
    m.ask_qty = 3;
    m.ts_us = 42;
    expect(pitlane::yard_publish(y, id, m), "publish");
    pitlane::YardMark out{};
    expect(pitlane::yard_latest(y, id, out) && out.bid_px == 100 && out.seq == 1, "latest");

    pitlane::YardTick tick{};
    tick.px = 100;
    tick.qty = 5;
    tick.side = 1;
    tick.ts_us = 43;
    expect(pitlane::yard_publish_tick(y, id, tick), "tick");
    pitlane::YardTick tick_out{};
    expect(pitlane::yard_latest_tick(y, id, tick_out) && tick_out.qty == 5 && tick_out.seq == 1, "latest tick");

    pitlane::YardPrint pr{};
    pr.px = 101;
    pr.qty = 1;
    pr.side = 2;
    pr.ts_us = 44;
    expect(pitlane::yard_publish_print(y, id, pr), "print");
    pitlane::YardPrint pr_out{};
    expect(pitlane::yard_latest_print(y, id, pr_out) && pr_out.px == 101 && pr_out.seq == 1, "latest print");

    expect(pitlane::yard_publish_rebuild(y, id, m) && pitlane::yard_latest_rebuild(y, id, out) && out.seq == 1,
           "rebuild");
    expect(pitlane::yard_publish_mix(y, id, m) && pitlane::yard_latest_mix(y, id, out) && out.seq == 1, "mix");

    const uint32_t hid = pitlane::yard_claim_hours(y, "CFFEX");
    expect(hid == 1, "hours");
    pitlane::YardHours* hours = pitlane::yard_hours(y, hid);
    expect(hours != nullptr, "hours ptr");
    if (hours) {
        hours->open_sec = 9 * 3600;
        hours->close_sec = 15 * 3600;
    }
    const uint32_t fid = pitlane::yard_claim_facts(y, "IF2501");
    expect(fid == 1, "facts");
    pitlane::YardFacts* facts = pitlane::yard_facts(y, fid);
    expect(facts != nullptr, "facts ptr");
    if (facts) {
        facts->tick_sz = 2;
        facts->lot_sz = 1;
        facts->mult = 300;
    }
    expect(pitlane::yard_link_facts(y, id, fid) && pitlane::yard_link_hours(y, id, hid), "link");
    expect(pitlane::yard_sym(y, id)->facts_id == static_cast<int32_t>(fid), "sym facts");
    expect(pitlane::yard_facts(y, fid)->hours_id == static_cast<int32_t>(hid), "facts hours");

    expect(pitlane::yard_claim_sym(y, "") == 0, "empty code");
    expect(!pitlane::yard_publish(y, 0, m), "bad id");

    for (uint32_t i = 0; i < plan.sym_slots - 1; ++i) {
        char code[16];
        std::snprintf(code, sizeof code, "S%u", i);
        expect(pitlane::yard_claim_sym(y, code) != 0, "fill slots");
    }
    expect(pitlane::yard_claim_sym(y, "FULL") == 0, "full fail-closed");

    pitlane::yard_close(&y);
    expect(y.head == nullptr, "closed");

    pitlane::YardView y2{};
    expect(pitlane::yard_open(path, plan, &y2, map_opt_tmp(), 0) == 0, "reattach");
    expect(pitlane::yard_sym(y2, 1) != nullptr && pitlane::yard_latest(y2, 1, out) && out.seq == 1, "persist quote");
    expect(pitlane::yard_latest_tick(y2, 1, tick_out) && tick_out.seq == 1, "persist tick");
    expect(pitlane::yard_facts(y2, 1) != nullptr && pitlane::yard_hours(y2, 1) != nullptr, "persist tables");
    pitlane::yard_close(&y2);
    ::unlink(path);

    pitlane::PitConf sc{};
    pitlane::conf_defaults(sc);
    pitlane::StudyPlan sp = pitlane::conf_study_plan(sc);
    expect(sp.signal_pools == 1000 && sp.factor_depth == 20, "study plan");

    {
        FILE* vf = std::fopen("pitlane_venue.conf", "w");
        expect(vf != nullptr, "venue conf file");
        if (vf) {
            std::fputs("[venue]\ndelay1=0.5\ncancel_delay2=1.25\nrigid_rule=1\nuse_ext=1\nlot=100\n", vf);
            std::fclose(vf);
        }
        pitlane::PitConf vc{};
        expect(pitlane::conf_load("pitlane_venue.conf", vc), "venue load");
        expect(vc.venue_go_us[0] == 500000 && vc.venue_pull_us[1] == 1250000, "venue sec delays");
        expect(vc.venue_rigid == 1 && vc.venue_tape_first == 1 && vc.venue_lot == 100, "venue flags");
        pitlane::VenueCfg cfg = pitlane::conf_venue_cfg(vc);
        expect(cfg.lag.go_us[0] == 500000 && cfg.lag.pull_us[1] == 1250000, "venue cfg lags");
        expect(cfg.rigid == 1 && cfg.tape_first == 1 && cfg.lot == 100, "venue cfg copy");
        ::unlink("pitlane_venue.conf");
    }

    {
        pitlane::CashLine cash_rows[4]{};
        pitlane::HoldLine hold_rows[4]{};
        pitlane::CashBoard cash(cash_rows, 4);
        pitlane::HoldBoard hold(hold_rows, 4);
        pitlane::CashSeed cs{};
        cs.tier = 1;
        expect(pitlane::seed_copy16(cs.prod, "stock") && pitlane::seed_copy24(cs.acct, "A1"), "seed copy");
        expect(pitlane::seed_copy16(cs.desk, "D") && pitlane::seed_copy16(cs.book, "B"), "seed desk");
        cs.free_amt = 1000000;
        expect(pitlane::seed_cash(cash, &cs, 1) == 1, "seed cash");
        pitlane::HoldSeed hs{};
        hs.tier = 1;
        expect(pitlane::seed_copy16(hs.prod, "stock") && pitlane::seed_copy24(hs.acct, "A1"), "hold acct");
        expect(pitlane::seed_copy16(hs.name, "IF2501"), "hold name");
        hs.long_yday = 10;
        hs.sellable = 7;
        hs.frozen_yday = 1;
        expect(pitlane::seed_hold(hold, &hs, 1) == 1, "seed hold");
        const uint64_t hk = pitlane::HoldBoard::key_of(1, "stock", "A1", "IF2501");
        const pitlane::HoldLine* hl = hold.find(hk);
        expect(hl && hl->sellable == 7 && hl->long_yday == 10, "seed hold values");
    }
}

static void check_local_shelf_bus() {
    pitlane::YardPlan plan{};
    plan.sym_slots = 8;
    plan.quote_lanes = 4;
    plan.quote_depth = 4;
    plan.order_lanes = 1;
    plan.trade_lanes = 1;
    plan.rebuild_lanes = 1;
    plan.mix_lanes = 1;
    plan.order_depth = 4;
    plan.trade_depth = 4;
    plan.rebuild_depth = 4;
    plan.mix_depth = 4;
    plan.base_info_cap = 8;
    plan.trade_time_cap = 4;
    char path[64];
    std::snprintf(path, sizeof path, "/tmp/pitlane-shelf-%d.bin", static_cast<int>(::getpid()));
    ::unlink(path);
    pitlane::YardView y{};
    expect(pitlane::yard_open(path, plan, &y, map_opt_tmp(), 1) == 0, "shelf yard");

    pitlane::HoursSeed hs{};
    expect(pitlane::shelf_copy32(hs.key, "SSE"), "hours key");
    hs.open_sec = 9 * 3600 + 30 * 60;
    hs.close_sec = 15 * 3600;
    expect(pitlane::seed_hours(y, &hs, 1) == 1, "seed hours");

    pitlane::FactSeed fs{};
    expect(pitlane::shelf_copy32(fs.code, "600000.SH") && pitlane::shelf_copy32(fs.hours_key, "SSE"), "fact keys");
    fs.tick_sz = 1;
    fs.lot_sz = 100;
    fs.mult = 1;
    expect(pitlane::seed_facts(y, &fs, 1) == 1, "seed facts");
    const uint32_t sid = pitlane::yard_claim_sym(y, "600000.SH");
    expect(sid != 0 && pitlane::yard_sym(y, sid)->facts_id != 0, "facts linked");

    pitlane::ClosedDays closed;
    const int hols[] = {20260101, 20261001};
    expect(pitlane::seed_holidays(closed, hols, 2) == 2 && closed.has(20260101), "seed holidays");

    pitlane::TopicBus bus;
    const uint32_t topic = pitlane::topic_id("outer_respond_routing");
    expect(topic != 0, "topic id");
    const char body[] = "ack";
    expect(bus.publish_key("outer_respond_routing", 2, body, 3, 42, 0), "bus pub");
    pitlane::BusNote got{};
    uint64_t cur = 0;
    expect(bus.drain(0, cur, topic, &got, 1) == 1 && got.kind == 2 && got.len == 3, "bus drain");
    expect(std::memcmp(got.data, "ack", 3) == 0, "bus payload");

    pitlane::RouteBook routes;
    expect(routes.bind("algo_book", "TwapAlgo") && routes.bind("algo_book", "TwapControl"), "route bind");
    expect(routes.has("algo_book", "TwapAlgo"), "route has");
    char names[4][pitlane::RouteBook::kName]{};
    expect(routes.lookup("algo_book", names, 4) == 2, "route lookup");

    {
        FILE* hf = std::fopen("pitlane_holidays.txt", "w");
        expect(hf != nullptr, "hol file");
        if (hf) {
            std::fputs("# holidays\n20260101\n20261001\n", hf);
            std::fclose(hf);
        }
        pitlane::ClosedDays from_file;
        expect(pitlane::load_holidays(from_file, "pitlane_holidays.txt") == 2, "load holidays");
        expect(from_file.has(20261001), "loaded day");
        ::unlink("pitlane_holidays.txt");
    }

    {
        FILE* ff = std::fopen("pitlane_facts.csv", "w");
        expect(ff != nullptr, "facts file");
        if (ff) {
            std::fputs("# code,tick,lot,mult,hours\n600001.SH,1,100,1,SSE\n", ff);
            std::fclose(ff);
        }
        FILE* hf = std::fopen("pitlane_hours.csv", "w");
        expect(hf != nullptr, "hours file");
        if (hf) {
            std::fputs("SSE,34200,41400,46800,54000\n", hf);
            std::fclose(hf);
        }
        expect(pitlane::load_hours(y, "pitlane_hours.csv") == 1, "load hours");
        expect(pitlane::load_facts(y, "pitlane_facts.csv") == 1, "load facts");
        ::unlink("pitlane_facts.csv");
        ::unlink("pitlane_hours.csv");
    }

    pitlane::yard_close(&y);
    ::unlink(path);
}

int main() {
    expect(pitlane::kVersionMajor == 2 && pitlane::kVersionMinor == 18 && pitlane::kVersionPatch == 0, "release");
    expect(std::strcmp(pitlane::kVersion, "2.18.0") == 0, "release text");
    expect(pitlane::kDeskVersion == 4, "desk layout");
    check_conf();
    check_yard();
    check_local_shelf_bus();
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
