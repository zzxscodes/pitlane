#include "adapters/ctp/book.hpp"
#include "adapters/ctp/decode.hpp"
#include "adapters/ctp/fanout.hpp"
#include "adapters/market_view.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

static int g_fails = 0;

static void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

static pitlane::CtpRaw make_raw() {
    pitlane::CtpRaw r{};
    std::snprintf(r.instrument, sizeof r.instrument, "rb2601");
    std::snprintf(r.exchange, sizeof r.exchange, "SHFE");
    std::snprintf(r.trading_day, sizeof r.trading_day, "20261117");
    std::snprintf(r.action_day, sizeof r.action_day, "20261117");
    std::snprintf(r.update_time, sizeof r.update_time, "09:00:00");
    r.update_ms = 500;
    r.last_px = 3800.0;
    r.pre_close = 3790.0;
    r.open_px = 3795.0;
    r.high_px = 3810.0;
    r.low_px = 3790.0;
    r.volume = 1000;
    r.turnover = 3800000.0;
    r.open_interest = 50000;
    r.upper_limit = 4100.0;
    r.lower_limit = 3450.0;
    r.bid_px[0] = 3799.0;
    r.bid_qty[0] = 7;
    r.bid_px[1] = 3798.0;
    r.bid_qty[1] = 9;
    r.ask_px[0] = 3801.0;
    r.ask_qty[0] = 4;
    r.ask_px[1] = 3802.0;
    r.ask_qty[1] = 6;
    for (int i = 2; i < pitlane::kCtpLevels; ++i) {
        r.bid_px[i] = 1e300; // 无效价
        r.ask_px[i] = 1e300;
    }
    return r;
}

static void check_decode() {
    expect(pitlane::ctp_px(3800.0) == 38000000, "px scale");
    expect(pitlane::ctp_px(0) == 0, "px zero");
    expect(pitlane::ctp_px(1e300) == 0, "px invalid");
    expect(pitlane::ctp_time_us("09:00:00", 500) == 32400500000ll, "time us");
    expect(pitlane::ctp_time_us("", 0) == 0, "time empty");

    pitlane::CtpSnap s{};
    const pitlane::CtpRaw raw = make_raw();
    expect(pitlane::ctp_decode(raw, s), "decode ok");
    expect(s.exch_us == 32400500000ll, "decode time");
    expect(s.last_px == 38000000, "decode last");
    expect(s.n_bid == 2 && s.n_ask == 2, "decode depth");
    expect(s.bid[0].px == 37990000 && s.bid[0].qty == 7, "decode bid1");
    expect(s.ask[0].qty == 4, "decode ask1");
    expect(s.status == pitlane::kCtpTrading, "decode status trading");
    expect(s.crossed == 0, "decode crossed");
    expect(s.upper_limit == 41000000, "decode limit up");

    pitlane::CtpRaw at = raw;
    at.last_px = at.upper_limit;
    expect(pitlane::ctp_decode(at, s) && s.status == pitlane::kCtpLimitUp, "limit up");
    at.last_px = at.lower_limit;
    expect(pitlane::ctp_decode(at, s) && s.status == pitlane::kCtpLimitDown, "limit down");
    at.last_px = 0;
    expect(pitlane::ctp_decode(at, s) && s.status == pitlane::kCtpAuction, "auction");

    pitlane::CtpRaw bad = raw;
    std::snprintf(bad.update_time, sizeof bad.update_time, "--:--:--");
    expect(!pitlane::ctp_decode(bad, s), "decode bad time");
}

static void check_print_maker() {
    pitlane::CtpPrintMaker maker;
    pitlane::CtpRaw raw = make_raw();
    pitlane::CtpSnap s{};
    pitlane::CtpPrint p[2]{};

    expect(pitlane::ctp_decode(raw, s), "pm decode 1");
    expect(maker.on_snap(s, p, 2) == 0, "pm first snapshot no print");

    raw.volume = 1005;
    raw.turnover = 3800000.0 + 5 * 3800.0;
    raw.last_px = 3801.0; // 打到卖一
    expect(pitlane::ctp_decode(raw, s), "pm decode 2");
    expect(maker.on_snap(s, p, 2) == 1, "pm print emitted");
    expect(p[0].qty == 5, "pm qty");
    expect(p[0].px == 38000000, "pm px");
    expect(p[0].aggressor == 1, "pm aggressor buy");

    // 累计量回退：视为换日/重放，不产生成交。
    raw.volume = 3;
    raw.turnover = 11400.0;
    expect(pitlane::ctp_decode(raw, s), "pm decode 3");
    expect(maker.on_snap(s, p, 2) == 0, "pm rollback no print");
    expect(maker.volume() == 3, "pm rollback baseline");

    // 无变化。
    expect(pitlane::ctp_decode(raw, s) && maker.on_snap(s, p, 2) == 0, "pm no delta");
    raw.volume = 6;
    raw.turnover = 22800.0;
    expect(pitlane::ctp_decode(raw, s) && maker.on_snap(s, p, 2) == 1 && p[0].qty == 3, "pm after rollback");
}

static void check_book() {
    pitlane::CtpRaw raw = make_raw();
    pitlane::CtpSnap s{};
    pitlane::CtpBookEvent ev = pitlane::kCtpBookOk;
    pitlane::CtpDepthBook book;

    expect(pitlane::ctp_decode(raw, s), "book decode");
    expect(book.apply(s, &ev) && ev == pitlane::kCtpBookOk, "book first");
    pitlane::CtpLevel bid{}, ask{};
    expect(book.best(bid, ask) && bid.px == 37990000 && ask.px == 38010000, "book best");
    expect(book.count(1) == 2 && book.count(2) == 2, "book counts");
    expect(book.imbalance_ppm() == (7 - 4) * 1000000 / 11, "book imbalance");

    // 卖一量变化 + 新档位：一增一改。
    const pitlane::CtpBookStats before = book.stats();
    raw.ask_qty[0] = 8;
    raw.ask_px[2] = 3803.0;
    raw.ask_qty[2] = 11;
    raw.update_ms = 700;
    expect(pitlane::ctp_decode(raw, s), "book decode 2");
    expect(book.apply(s, &ev), "book apply 2");
    expect(book.stats().inserts - before.inserts == 1 && book.stats().updates - before.updates >= 2, "book diff stats");
    expect(book.count(2) == 3, "book depth grew");

    // 时间戳回退：整簿重建并标记 stale。
    raw.update_ms = 600;
    expect(pitlane::ctp_decode(raw, s), "book decode 3");
    expect(book.apply(s, &ev) && ev == pitlane::kCtpBookStale, "book stale");
    expect(book.stats().stale == 1 && book.stats().rebuilds == 1, "book stale stats");

    // 交叉盘口被标记。
    raw.update_ms = 800;
    raw.bid_px[0] = 3802.0;
    expect(pitlane::ctp_decode(raw, s), "book decode 4");
    expect(book.apply(s, &ev) && ev == pitlane::kCtpBookCrossed, "book crossed");

    // 品种编号随订单簿绑定，控制台的行情快照靠它区分合约。
    pitlane::CtpBookSet set;
    pitlane::CtpDepthBook* bound = set.ensure(7);
    expect(bound && bound->name() == 7, "bookset bind");
    pitlane::CtpBookView view{};
    bound->copy(view);
    expect(view.name == 7, "book view name");
}

// 目录容量对齐：1024 个品种各自拿到独立订单簿，越界编号被拒绝而不是静默丢弃。
static void check_bookset_capacity() {
    std::unique_ptr<pitlane::CtpBookSet> set(new pitlane::CtpBookSet());
    for (uint32_t name = 1; name <= static_cast<uint32_t>(pitlane::CtpBookSet::kNames); ++name) {
        pitlane::CtpDepthBook* b = set->ensure(name);
        expect(b && b->name() == name, "bookset full capacity bind");
        if (!b) return;
    }
    expect(set->count() == pitlane::CtpBookSet::kNames, "bookset full count");
    expect(set->name_at(0) == 1 && set->name_at(set->count() - 1) == pitlane::CtpBookSet::kNames, "bookset order");
    expect(set->ensure(1) == set->find(1), "bookset idempotent");
    expect(set->find(0) == nullptr, "bookset rejects name 0");
    expect(set->ensure(static_cast<uint32_t>(pitlane::CtpBookSet::kSlots)) == nullptr, "bookset rejects overflow");
    expect(set->name_at(-1) == 0 && set->name_at(set->count()) == 0, "bookset name_at bounds");
}

static bool sink_ok_snap(void*, const pitlane::CtpSnap&) { return true; }
static bool sink_ok_print(void*, const pitlane::CtpPrint&) { return true; }
static bool sink_no_snap(void*, const pitlane::CtpSnap&) { return false; }
static bool sink_no_print(void*, const pitlane::CtpPrint&) { return false; }

static void check_fanout() {
    pitlane::CtpFanout fan;
    int a = 1, b = 2;
    const int ia = fan.add(&a, sink_ok_snap, sink_ok_print);
    const int ib = fan.add(&b, sink_no_snap, sink_no_print);
    expect(ia == 0 && ib == 1, "fan add");

    pitlane::CtpRaw raw = make_raw();
    pitlane::CtpSnap s{};
    expect(pitlane::ctp_decode(raw, s), "fan decode");
    pitlane::CtpPrint p{};
    p.qty = 1;
    expect(fan.publish(s, &p, 1) == 1, "fan one sink ok");
    expect(fan.stats(0).snaps == 1 && fan.stats(0).prints == 1, "fan sink0 stats");
    expect(fan.stats(1).dropped == 1, "fan sink1 dropped");
    expect(fan.publish_print(p) == 1, "fan print only");
}

static void check_live() {
    pitlane::MarketBoard board;
    pitlane::MarketSnap in{};
    in.decoded = 3;
    in.books = 1;
    in.book[0].name = 7;
    in.book[0].levels = 5;
    in.book[0].bid[0].px = 10000;
    in.book[0].bid[0].qty = 12;
    in.book[0].imbalance_ppm = -500000;
    in.print_n = 1;
    in.print[0].name = 7;
    in.print[0].px = 10001;
    in.print[0].qty = 2;
    board.publish(in);

    pitlane::MarketSnap out{};
    expect(board.read(out), "live read");
    expect(out.decoded == 3 && out.books == 1, "live counters");
    expect(out.book[0].name == 7 && out.book[0].levels == 5, "live book");
    expect(out.book[0].bid[0].px == 10000 && out.book[0].bid[0].qty == 12, "live level");
    expect(out.book[0].imbalance_ppm == -500000, "live imbalance");
    expect(out.print_n == 1 && out.print[0].px == 10001, "live print");
    expect(board.version() == 2, "live version");
}

int main() {
    check_decode();
    check_print_maker();
    check_book();
    check_bookset_capacity();
    check_fanout();
    check_live();
    if (g_fails) {
        std::fprintf(stderr, "%d ctp checks failed\n", g_fails);
        return 1;
    }
    std::printf("ctp ok\n");
    return 0;
}
