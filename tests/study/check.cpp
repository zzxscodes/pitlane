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

void check_feat_and_mark() {
    pitlane::BookTop top;
    top.bid_px = 100;
    top.ask_px = 102;
    top.bid_qty = 1;
    top.ask_qty = 3;
    pitlane::Feat feat;
    expect(pitlane::feat_of(top, feat) && feat.spread == 2 && feat.mid == 101 && feat.imbalance == -2, "feat");
    expect(feat.micro == 100, "micro");
    pitlane::Horizon horizon;
    horizon.entry = 100;
    horizon.side = 1;
    horizon.later[0] = 110;
    horizon.later[1] = 90;
    horizon.later[2] = -1;
    int64_t ticks[4] = {};
    expect(pitlane::horizon_ticks(horizon, 2, ticks) && ticks[0] == 5 && ticks[1] == -5 && ticks[2] == 0, "buy mark");
    horizon.side = 2;
    expect(pitlane::horizon_ticks(horizon, 2, ticks) && ticks[0] == -5 && ticks[1] == 5, "sell mark");
}

void check_tables() {
    pitlane::RelBook rel;
    expect(rel.bind(2, 1) && rel.bind(3, 1) && rel.underlying(2) == 1, "bind");
    uint32_t names[4] = {};
    expect(rel.related(1, names, 4) == 2 && names[0] == 2 && names[1] == 3, "related");
    expect(!rel.bind(0, 1), "bad name");

    pitlane::FactorBoard board;
    int64_t value = 0;
    expect(board.put(9, 0, 40) && board.put(9, 1, -2) && board.get(9, 0, value) && value == 40, "factor");
    expect(board.get(9, 1, value) && value == -2 && !board.get(9, 2, value), "empty slot");

    const int rows[] = {10, 20, 30};
    pitlane::Replay<int> replay;
    replay.data = rows;
    replay.n = 3;
    const int* item = nullptr;
    expect(replay.take(item) && *item == 10 && replay.take(item) && *item == 20, "replay");
    replay.reset();
    expect(replay.take(item) && *item == 10, "reset");
}

bool choose_buy(const pitlane::Feat& feat, const pitlane::BookTop& top, const int64_t* factor, int nfactor,
                pitlane::Idea& idea, void*) {
    if (feat.spread != 1 || nfactor < 1 || !factor || factor[0] < 0) return false;
    idea.px = top.ask_px;
    idea.qty = 1;
    idea.side = 1;
    return true;
}

bool choose_no(const pitlane::Feat&, const pitlane::BookTop&, const int64_t*, int, pitlane::Idea&, void*) { return false; }

void check_step() {
    pitlane::QuoteFold<8> book;
    const pitlane::FoldEdit edits[2] = {
        {pitlane::FoldOp::Set, 1, 100, 4},
        {pitlane::FoldOp::Set, 2, 101, 3},
    };
    const int64_t factor[] = {7};
    pitlane::Feat feat;
    pitlane::BookTop top;
    pitlane::GateAsk gate;
    gate.mode = pitlane::PxMode::Limit;
    gate.off = pitlane::Off::Open;
    gate.last = 100;
    gate.dev_den = 100;
    gate.dev_cap = 50;
    pitlane::Sheet sheet;
    const pitlane::Cause cause =
        pitlane::study_step(book, edits, 2, factor, 1, choose_buy, nullptr, &gate, &sheet, 8, 1000, feat, top);
    expect(cause == pitlane::Cause::Ok && sheet.phase == pitlane::Phase::Staged && sheet.px == 101 && feat.spread == 1,
           "staged");

    pitlane::Sheet held = sheet;
    expect(pitlane::study_step(book, edits, 2, factor, 1, choose_no, nullptr, &gate, &sheet, 8, 1000, feat, top) ==
               pitlane::Cause::Ok &&
           sheet.phase == held.phase && sheet.px == held.px,
           "no order");

    pitlane::NameList names;
    expect(names.add("ZZ"), "list");
    gate.names = &names;
    gate.symbol = "ZZ";
    pitlane::Sheet fresh;
    const pitlane::Cause blocked =
        pitlane::study_step(book, edits, 2, factor, 1, choose_buy, nullptr, &gate, &fresh, 9, 1000, feat, top);
    expect(blocked == pitlane::Cause::Blocked && fresh.local == 0, "blocked");
}

static void check_corp() {
    const pitlane::CorpAct acts[] = {
        {20240102, 1, 2, 100, 0, 0},
        {20240103, 1, 1, 100, 10, 0},
    };
    pitlane::PxMul factor{};
    int64_t px = 0;
    expect(pitlane::mul_fold(acts, 2, 20240101, factor) && pitlane::scale_px(50, factor, px) && px == 50, "before the split");
    expect(pitlane::mul_fold(acts, 2, 20240102, factor) && pitlane::scale_px(50, factor, px) && px == 100, "after the split");
    expect(pitlane::mul_fold(acts, 2, 20240103, factor) && pitlane::scale_px(50, factor, px) && px == 100, "price-only dividend");
    int64_t num = 0;
    int64_t den = 1;
    expect(pitlane::traded_move(100, 50, 1, 2, num, den) && num == 0, "the split itself is not a traded move");
    expect(pitlane::traded_move(100, 60, 1, 2, num, den) && num == 1 && den == 5, "traded move after the jump");
    pitlane::CorpAct income{20240103, 1, 1, 100, 10, 1};
    pitlane::PxMul step{};
    expect(pitlane::act_mul(income, step) && step.num == 10 && step.den == 9, "income dividend");
}

static void check_decide() {
    pitlane::DecideScratch d;
    pitlane::decide_clear(d);
    expect(pitlane::decide_arm(d, 100, 2, 1, 5), "arm");
    expect(!pitlane::decide_prefer(d, 101, 2, 1, 4), "worse score");
    expect(pitlane::decide_prefer(d, 101, 2, 1, 9), "better score");
    pitlane::Idea idea{};
    expect(pitlane::decide_take(d, idea) && idea.px == 101 && idea.side == 1, "take");
    pitlane::BookTop top{};
    top.bid_px = 99;
    top.ask_px = 100;
    top.bid_qty = 1;
    top.ask_qty = 1;
    expect(pitlane::decide_from_top(top, true, 1, idea) && idea.px == 100, "from ask");
}

static void check_plan() {
    pitlane::DagPlan<4> dag;
    dag.clear();
    expect(dag.add_edge(0, 1) && dag.add_edge(1, 2) && dag.build(0), "dag");
    expect(dag.size() == 3 && dag.at(0) == 2 && dag.at(2) == 0, "order");
    int seen = 0;
    expect(dag.run([&](int node) {
        ++seen;
        (void)node;
        return true;
    }) && seen == 3,
           "run");
    dag.clear();
    expect(dag.add_edge(0, 1) && dag.add_edge(1, 0) && !dag.build(0) && dag.cycle(), "cycle");
    pitlane::ActPlan<int, 4> acts;
    acts.step[0] = 7;
    acts.step[1] = 8;
    acts.n = 2;
    int sum = 0;
    expect(acts.run([&](int a) {
        sum += a;
        return true;
    }) && sum == 15,
           "act plan");
}

static void check_extreme() {
    pitlane::ExtremeWindow<4> trail;
    int64_t hi = 0;
    int64_t lo = 0;
    expect(!trail.high(hi) && !trail.low(lo), "empty trail");
    expect(trail.push(10) && trail.push(1) && trail.push(2) && trail.push(3), "fill trail");
    expect(trail.high(hi) && hi == 10 && trail.low(lo) && lo == 1 && trail.size() == 4, "first window");
    expect(!trail.push(0) && trail.high(hi) && hi == 10, "non-positive stays out");
    expect(trail.push(4) && trail.high(hi) && hi == 4 && trail.low(lo) && lo == 1, "oldest high leaves");
    expect(trail.push(9) && trail.high(hi) && hi == 9 && trail.low(lo) && lo == 2, "oldest low leaves");
    expect(trail.push(5) && trail.push(5) && trail.high(hi) && hi == 9 && trail.low(lo) && lo == 4, "equal prices");
    trail.clear();
    expect(trail.size() == 0 && !trail.high(hi), "cleared");
}

static void check_right() {
    expect(pitlane::right_value(1, 110, 100) == 10 && pitlane::right_value(2, 110, 100) == 0, "call");
    expect(pitlane::right_value(2, 90, 100) == 10 && pitlane::right_value(3, 90, 100) == -1, "put");
}

static void check_flow() {
    pitlane::TopSnap a{};
    a.bid_px = 100;
    a.ask_px = 101;
    a.bid_sz = 5;
    a.ask_sz = 5;
    pitlane::TopSnap b = a;
    b.bid_sz = 8;
    expect(pitlane::ofi_step(a, b) == 3, "ofi size up");
    pitlane::OfiWin win;
    pitlane::ofi_on(win, a);
    pitlane::ofi_on(win, b);
    expect(win.sum == 3, "ofi win");
    expect(pitlane::agg_from_seq(3, 1) == pitlane::Agg::Buy, "agg");
    pitlane::FootBin foot;
    pitlane::foot_add(foot, pitlane::Agg::Buy, 2);
    pitlane::foot_add(foot, pitlane::Agg::Sell, 1);
    expect(pitlane::foot_delta(foot) == 1 && pitlane::foot_total(foot) == 3, "foot");
    pitlane::SecMove<8> move;
    move.push(1000, 100);
    move.push(2000, 101);
    int ppm = 0;
    expect(move.move_ppm(ppm, 2000, 500) && ppm == 100, "sec move");
    pitlane::TradeMidGap<4> gaps;
    expect(gaps.push(105, 100) && gaps.push(95, 100), "gaps");
    int64_t mean = 0;
    expect(gaps.mean(mean) && mean == 0, "gap mean");
}

static void check_acb() {
    pitlane::FifoLot<4> fifo;
    expect(fifo.buy(100, 2) == pitlane::LotPnL::Ok && fifo.lots() == 2, "fifo buy");
    int64_t part = 0;
    expect(fifo.sell(110, 1, &part) == pitlane::LotPnL::Ok && part == 10 && fifo.realized() == 10, "fifo sell");
    expect(fifo.sell(90, 2) == pitlane::LotPnL::Short, "fifo short");

    pitlane::AcbBook book{};
    expect(pitlane::acb_apply(book, 100, 2) && book.pos == 2 && book.suma == 200, "acb open");
    expect(pitlane::acb_apply(book, 110, -1) && book.pos == 1 && book.rpnl == 10, "acb sell");
    expect(pitlane::acb_upnl(book, 120) == 20, "acb upnl");
    expect(pitlane::acb_apply(book, 90, -2) && book.pos == -1 && book.suma == -90, "acb flip short");
}

static void check_fair() {
    expect(pitlane::fair_from_top(100, 2, 102, 2) == 101, "fair mid");
    expect(pitlane::book_bias_ppm(1, 3) == 500000, "bias");
    pitlane::PressureEma p{};
    expect(pitlane::pressure_open(p, 1000000), "press open");
    expect(pitlane::pressure_sample(p, 103, 101, 2) && p.value == 1000000, "press sample");
    expect(pitlane::fair_score(100, 50, 1, 1) == 150, "score");
}

static void check_factorcal() {
    pitlane::FactorSeries<8> series{};
    expect(series.write_plain(0, 100, 7, 100, 0) && series.last_write == 0, "factor first");
    expect(series.write_plain(2, 300, 9, 100, 1), "factor gap");
    expect(series.at(1) && series.at(1)->filled == 1 && series.at(1)->value == 1, "gap filled");
    expect(series.at(2) && series.at(2)->filled == 0 && series.at(2)->value == 9, "real sample");
    expect(!series.write_plain(1, 200, 8, 100, 0), "reject rewind");

    pitlane::NamedFactorRing<4, 8> named{};
    auto* slot = named.open("rsi");
    expect(slot && named.find("rsi") == slot, "named open");
    expect(slot->series.write_plain(0, 100, 42, 100, 0), "named write");
    expect(!named.open("a_very_long_factor_name_that_exceeds"), "key too long");
}

static void check_shadow() {
    pitlane::ShadowStats stats;
    pitlane::ShadowPair p{};
    p.side = 1;
    p.real_px = 101;
    p.sim_px = 100;
    p.real_qty = 10;
    p.sim_qty = 10;
    p.tick = 1;
    stats.note(p); // buy paid 1 tick more: worse
    p.side = 2;
    p.real_px = 99;
    p.sim_px = 100;
    stats.note(p); // sell got 1 tick less: worse
    p.side = 1;
    p.real_px = 99;
    p.sim_px = 100;
    stats.note(p); // buy paid less: better
    p.real_qty = 0;
    p.sim_qty = 5;
    stats.note(p); // shadow filled, real did not
    p.real_qty = 5;
    p.sim_qty = 0;
    stats.note(p); // real filled, shadow did not

    pitlane::ShadowReport report{};
    expect(stats.report(report), "shadow report");
    expect(report.pairs == 5 && report.worse == 2 && report.better == 1 && report.missed == 1 &&
               report.extra == 1,
           "shadow verdicts");
    expect(report.slip_sum_ticks == 1 && report.slip_max_ticks == 1 && report.slip_n == 3 &&
               report.slip.p50 == 1,
           "shadow slippage");

    pitlane::ShadowBook book;
    const int twap = book.open("twap");
    expect(twap == 0, "shadow book row");
    expect(book.note(twap, p), "shadow book note");
    pitlane::ShadowReport row{};
    expect(book.report(twap, row) && row.pairs == 1, "shadow book report");
    expect(book.open("twap") == twap && book.open("vwap") == 1, "shadow book dedupe");
    expect(book.name(twap) && std::strcmp(book.name(twap), "twap") == 0, "shadow book name");
}

void check_perf() {
    pitlane::PerfCurve<8> curve;
    pitlane::PerfCurve<8>::Stats stats{};
    expect(!curve.result(stats), "perf empty");
    const int64_t eq[] = {0, 10, 5, 15, 3};
    for (int64_t v : eq) expect(curve.mark(v), "perf mark");
    expect(curve.result(stats), "perf result");
    expect(stats.n == 5 && stats.start == 0 && stats.end == 3 && stats.peak == 15 && stats.min == 0, "perf bounds");
    expect(stats.max_drawdown == 12, "perf maxdd");
    for (int i = 0; i < 3; ++i) expect(curve.mark(1), "perf fill");
    expect(!curve.mark(1), "perf full");
}

int main() {
    check_feat_and_mark();
    check_tables();
    check_step();
    check_corp();
    check_decide();
    check_plan();
    check_extreme();
    check_right();
    check_flow();
    check_acb();
    check_fair();
    check_factorcal();
    check_shadow();
    check_perf();
    if (g_fails) {
        std::fprintf(stderr, "%d study check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
