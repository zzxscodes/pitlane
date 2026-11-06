#include "pitlane/pitlane.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_fails = 0;

void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
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

void check_life_purse() {
    expect(pitlane::life_next(pitlane::Phase::None, pitlane::LifeEvent::Submit) == pitlane::Phase::Staged, "submit");
    expect(pitlane::life_next(pitlane::Phase::Staged, pitlane::LifeEvent::Accept) == pitlane::Phase::Working, "accept");
    expect(pitlane::life_next(pitlane::Phase::Working, pitlane::LifeEvent::Fill) == pitlane::Phase::Partial, "fill");
    expect(pitlane::life_next(pitlane::Phase::Partial, pitlane::LifeEvent::AskPull) == pitlane::Phase::PullWait, "ask pull");
    expect(pitlane::life_next(pitlane::Phase::PullWait, pitlane::LifeEvent::Fill) == pitlane::Phase::PullWait, "fill while pulling");
    expect(pitlane::life_next(pitlane::Phase::PullWait, pitlane::LifeEvent::ResumePartial) == pitlane::Phase::Partial, "resume");
    expect(pitlane::life_next(pitlane::Phase::Filled, pitlane::LifeEvent::Fill) == pitlane::Phase::None, "final rejects");

    const pitlane::ClassScale fut = pitlane::scale_of(pitlane::Product::Future);
    expect(pitlane::margin_need(fut, 10, 2) == 300, "future margin");
    expect(pitlane::charge_class(pitlane::Product::Stock, 10000, 3, true, 10, 1, 3, 10000) == 14, "stock charge");
    expect(pitlane::charge_class(pitlane::Product::Future, 10000, 3, true, 10, 1, 3, 10000) == 3, "future charge");

    expect(pitlane::rest_room(true, 5, 10, 1, 0, 0) == 0, "hit together rests nothing");
    expect(pitlane::rest_room(false, 5, 10, 2, 3, 1) == 5, "rest room capped");
    int64_t kept = 8;
    int64_t other = -6;
    pitlane::cut_hedge(kept, other, 5);
    expect(kept == 5 && other == -3, "hedge cut");
    int64_t slice = 0;
    expect(pitlane::slice_weight(1, 2, 3, 10, 0, slice) && slice == 2, "half rounds up");
    expect(pitlane::align_leg(250, 2, 100) == 100, "align below half");
    expect(pitlane::align_leg(300, 2, 100) == 200, "align at half");
    expect(pitlane::pair_qty_ok(true, "688001", 180) && !pitlane::pair_qty_ok(true, "688001", 179), "tech lot");
    expect(pitlane::pair_qty_ok(true, "600000", 80) && pitlane::pair_qty_ok(false, "600000", 1), "board lot");
    const pitlane::PxBand bands[] = {{0, 10000, 10}};
    expect(pitlane::chase_px(true, 100, 2, bands, 1, 0, 0) == 120, "chase bid up");

    pitlane::Wide w = pitlane::mul_wide(10000000000ull, 10000000000ull);
    uint64_t q = 0;
    expect(pitlane::div_wide(w, 10000000000ull, q) && q == 10000000000ull, "wide divide");
}

void check_bind_lane() {
    pitlane::IdBind<4> bind;
    pitlane::CounterKey key{};
    key.kind = 1;
    std::memcpy(key.text, "OID1", 4);
    std::memcpy(key.api, "front", 5);
    expect(bind.put(key, 42), "link");
    uint64_t local = 0;
    expect(bind.find(key, &local) && local == 42, "find counter");
    pitlane::CounterKey back{};
    expect(bind.find_local(42, &back) && back.num == 0 && std::memcmp(back.text, "OID1", 4) == 0, "find local");
    expect(bind.confirm(key), "confirm");
    expect(bind.erase(key) && !bind.find(key, &local), "erase");
    expect(bind.put(key, 7), "reuse tomb");

    pitlane::BindOrder order{};
    order.phase = pitlane::BindPhase::Sent;
    pitlane::bind_fill(order, 2, 10);
    expect(order.filled_qty == 0 && order.early_qty == 2 && order.early_amt == 20, "fill before status");
    pitlane::bind_status(order, pitlane::BindPhase::Acked);
    expect(order.phase == pitlane::BindPhase::Partial && order.filled_qty == 2 && order.early_qty == 0, "status merges");

    pitlane::BindOrder early{};
    early.phase = pitlane::BindPhase::Acked;
    pitlane::bind_final(early, pitlane::BindPhase::Filled, 0, 3);
    expect(early.phase == pitlane::BindPhase::Acked && early.early_phase == pitlane::BindPhase::Filled, "final waits");
    pitlane::bind_catchup(early, 3, 10, 0);
    expect(early.phase == pitlane::BindPhase::Filled && early.filled_qty == 3, "catch up");

    pitlane::QuoteHold quote{};
    quote.qty = 5;
    quote.px = 10;
    quote.cancel_out = 1;
    pitlane::quote_fill(quote, 2, 10);
    expect(quote.qty == 3 && quote.filled == 2 && !pitlane::quote_repost(quote, 10) && pitlane::quote_repost(quote, 11),
           "same price waits");

    pitlane::LaneRule rule{};
    rule.market = 0;
    rule.limit = 1;
    rule.pull = 1;
    expect(!pitlane::lane_accepts(rule, pitlane::PxMode::Market, pitlane::Off::Open, pitlane::Act::Enter), "no market");
    expect(pitlane::lane_accepts(rule, pitlane::PxMode::Limit, pitlane::Off::Open, pitlane::Act::Enter), "limit open");
    expect(pitlane::lane_accepts(rule, pitlane::PxMode::Limit, pitlane::Off::Open, pitlane::Act::Pull), "pull");
    expect(pitlane::cause_band(pitlane::Cause::WashRisk) == pitlane::CauseBand::Risk, "risk band");
    expect(pitlane::cause_band(pitlane::Cause::Venue) == pitlane::CauseBand::External, "external band");
    expect(std::strcmp(pitlane::cause_text(pitlane::Cause::NoCash), "cash") == 0, "cause text");
}

void check_judge() {
    pitlane::NameList opens;
    pitlane::NameList closes;
    expect(opens.add("AAA") && !opens.add("AAA"), "list once");
    expect(pitlane::list_cause(opens, closes, true, "AAA") == pitlane::Cause::Blocked, "open block");
    expect(pitlane::list_cause(opens, closes, false, "AAA") == pitlane::Cause::Ok, "close list is separate");

    pitlane::FlowWindow flow;
    expect(flow.allow(1000, 2, 1000), "flow room");
    expect(flow.add(1000, 1000), "flow add");
    expect(!flow.allow(1500, 2, 1000), "flow full");
    expect(flow.allow(2500, 2, 1000), "flow window moved");

    pitlane::PullTally pull{};
    pull.placed = 3;
    pull.pulled = 1;
    expect(pitlane::count_ratio_ok(pull, 100, 1, 2), "small sample skipped");
    pull.placed = 9;
    pull.pulled = 4;
    expect(pitlane::count_ratio_ok(pull, 1, 1, 2), "equal count ratio passes");
    pull.pulled = 5;
    expect(!pitlane::count_ratio_ok(pull, 1, 1, 2), "count ratio over");
    pull.placed_cash = 20;
    pull.pulled_cash = 10;
    pull.pulls_in_window = 3;
    expect(!pitlane::cash_ratio_ok(pull, 1, 1, 2), "equal cash ratio fails");

    pitlane::DayTally day{};
    day.orders = 5;
    day.cancels = 1;
    day.rejects = 1;
    expect(pitlane::cancel_ok(day, 1, 2), "cancel ratio");
    day.fill_cash = 50;
    day.order_cash = 100;
    expect(pitlane::fill_ok(day, 1, 2), "fill floor");
    day.buy_fill = 8;
    day.sell_fill = 3;
    expect(pitlane::net_buy(day) == 5, "net buy");
    expect(pitlane::px_shift(true, 110, 100, 100) == 10, "buy shift");
    expect(pitlane::deviation_cause(true, 110, 100, 100, 9) == pitlane::Cause::BadPrice, "shift over");

    pitlane::WashRow rows[2]{};
    rows[0].acct = 7;
    rows[0].group = 3;
    rows[0].name = 1;
    rows[0].px = 10;
    rows[0].side = 2;
    rows[0].live = 1;
    expect(pitlane::wash_cause(rows, 1, 8, 3, 1, 1, 10) == pitlane::Cause::WashRisk, "group wash");
    expect(pitlane::wash_cause(rows, 1, 8, 0, 1, 1, 10) == pitlane::Cause::Ok, "other account");
    expect(pitlane::wash_cause(rows, 1, 7, 0, 1, 1, 9) == pitlane::Cause::Ok, "does not cross");
}

void check_frame_span() {
    uint8_t buf[32]{};
    pitlane::ByteFrame frame;
    expect(frame.begin(buf, 32, 7), "begin");
    const char body[] = "ab";
    expect(frame.put(body, 2), "put");
    expect(frame.end() == 6, "end");
    pitlane::FrameView view{};
    expect(pitlane::frame_read(buf, 6, view) && view.kind == 7 && view.len == 2 && view.body[0] == 'a', "read");
    expect(!pitlane::frame_read(buf, 5, view), "truncated");

    pitlane::SeqWatch watch{};
    uint64_t missed = 0;
    expect(pitlane::seq_feed(watch, 5, &missed) == 0 && watch.next == 6, "first seq");
    expect(pitlane::seq_feed(watch, 8, &missed) == 2 && missed == 2, "gap");
    expect(pitlane::seq_feed(watch, 8, &missed) == -1, "old seq");

    pitlane::SeqHold<4, 8> hold;
    const uint8_t a[] = {1};
    const uint8_t b[] = {2};
    expect(hold.start(1), "start");
    expect(hold.offer(2, b, 1) && hold.offer(1, a, 1), "hold ahead");
    uint8_t out[8]{};
    uint64_t seq = 0;
    expect(hold.take(out, 8, &seq) == 1 && seq == 1 && out[0] == 1, "take first");
    expect(hold.take(out, 8, &seq) == 1 && seq == 2 && out[0] == 2, "take next");
    expect(hold.take(out, 8, &seq) == 0, "waiting");

    pitlane::SpanMark<4> span;
    expect(span.mark(0, 10) && span.mark(2, 25), "mark");
    int64_t dt = 0;
    expect(span.between(0, 2, dt) && dt == 15, "between");
    expect(!span.between(0, 1, dt), "unset node");
}

}  // namespace

int main() {
    check_quote();
    check_walk();
    check_life_purse();
    check_bind_lane();
    check_judge();
    check_frame_span();
    if (g_fails) {
        std::fprintf(stderr, "%d assist check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
