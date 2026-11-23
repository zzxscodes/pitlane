#include "pitlane/pitlane.hpp"
#include "pitlane/study/backtest.hpp"
#include "pitlane/study/block_tape.hpp"
#include "pitlane/study/factor_lab.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/stat.h>
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

static void check_calendar() {
    const int64_t regular[] = {100, 200, 300, 400};
    pitlane::CalendarMap<8> map{};
    expect(map.reset(regular, 4) && map.regular() && map.step() == 100, "calendar regular");
    expect(map.index_of(300) == 2 && map.index_of(450) == -1 && map.index_of(50) == -1, "calendar exact");
    expect(map.floor_index(350) == 2 && map.ceil_index(350) == 3, "calendar bounds");

    const int64_t broken[] = {100, 200, 400, 500};
    pitlane::CalendarMap<8> irregular{};
    expect(irregular.reset(broken, 4) && !irregular.regular(), "calendar irregular");
    expect(irregular.index_of(400) == 2 && irregular.floor_index(300) == 1 &&
               irregular.ceil_index(300) == 2,
           "calendar search");

    pitlane::CalendarFactorStore<4, 8, 8> store{};
    expect(store.reset(regular, 4), "store reset");
    expect(store.write("rsi", 100, 10, 0), "store first");
    expect(store.write_carry("rsi", 300, 30), "store carry");
    const pitlane::FactorCell* gap = store.at("rsi", 200);
    expect(gap && gap->value == 10 && gap->filled == 1, "store filled by slot");
    expect(store.at("rsi", 300) && store.at("rsi", 300)->value == 30, "store exact");
    expect(!store.write("rsi", 200, 20, 0), "store rejects rewind");
    expect(!store.write("rsi", 450, 20, 0), "store rejects off-grid");
    expect(store.latest("rsi") && store.latest("rsi")->value == 30 && store.last_time("rsi") == 300,
           "store latest");

    pitlane::CalendarFactorStore<4, 8, 8> broken_store{};
    expect(broken_store.reset(broken, 4), "irregular store reset");
    expect(broken_store.write_carry("mom", 100, 5) && broken_store.write_carry("mom", 500, 9),
           "irregular writes");
    const pitlane::FactorCell* broken_gap = broken_store.at("mom", 400);
    expect(broken_gap && broken_gap->value == 5 && broken_gap->filled == 1, "irregular gap");
    expect(!broken_store.write("mom", 300, 7, 0), "irregular off-grid");
}

static void check_faces() {
    pitlane::NamedRing<pitlane::NamedCell, 4> ring{};
    expect(ring.open("fast"), "ring open");
    pitlane::NamedCell cell{};
    cell.reading = 1;
    expect(ring.push(cell), "ring push");
    cell.reading = 2;
    expect(ring.push(cell), "ring push");
    uint64_t cursor = 0;
    pitlane::NamedCell out{};
    expect(ring.take(cursor, out) && out.reading == 1, "ring take first");
    expect(ring.take(cursor, out) && out.reading == 2, "ring take second");
    expect(!ring.take(cursor, out), "ring empty");
    expect(ring.latest(out) && out.reading == 2, "ring latest");
    for (int i = 0; i < 6; ++i) {
        cell.reading = 100 + i;
        expect(ring.push(cell), "ring overwrite");
    }
    uint64_t behind = cursor;
    uint64_t lost = 0;
    expect(ring.take(behind, out, &lost) && lost == 2 && behind == 5 && out.reading == 102,
           "ring reports lost cells");
    expect(!ring.open("a_name_that_is_far_too_long_for_the_slot"), "ring key limit");

    pitlane::FacePool<2, 2, 2, 2, 4, 4, 4> pool{};
    pitlane::InstrumentFaces<4, 4, 4> face{};
    expect(pool.bind(face) && face.index == 0, "pool bind");
    pitlane::InstrumentFaces<4, 4, 4> second{};
    expect(pool.bind(second) && second.index == 1, "pool second");
    pitlane::InstrumentFaces<4, 4, 4> third{};
    expect(!pool.bind(third), "pool full");

    const bool a = face.factor[0].open("a");
    const bool b = face.factor[1].open("b");
    expect(a && b, "face names");
    pitlane::NamedCell va{};
    va.reading = 11;
    expect(face.factor[0].push(va), "face a");
    va.reading = 22;
    expect(face.factor[1].push(va), "face b");

    uint64_t cursors[2] = {};
    pitlane::FaceRow<pitlane::NamedCell> rows[4] = {};
    const int n = pitlane::drain_named(face.factor, 2, cursors, rows, 4);
    expect(n == 2 && std::strcmp(rows[0].key, "a") == 0 && rows[0].cell.reading == 11 &&
               std::strcmp(rows[1].key, "b") == 0 && rows[1].cell.reading == 22,
           "drain named");
    expect(pitlane::drain_named(face.factor, 2, cursors, rows, 4) == 0, "drain empty");

    pitlane::OpenSignal sig{};
    sig.reading = 7;
    sig.sent_ns = 99;
    std::strcpy(sig.label, "open");
    expect(face.open_sig[0].open("open") && face.open_sig[0].push(sig), "open signal");
    pitlane::OpenSignal got{};
    uint64_t open_cursor = 0;
    expect(face.open_sig[0].take(open_cursor, got) && got.reading == 7 && got.sent_ns == 99, "open take");
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

static constexpr int kTapeN = 600;

static void make_tape(pitlane::Quote* tape, int n) {
    for (int i = 0; i < n; ++i) {
        const int64_t trend =
            i < n / 2 ? static_cast<int64_t>(i) * 200 : static_cast<int64_t>(n - i) * 200;
        const int64_t px = 1000000 + trend;
        pitlane::Quote& q = tape[i];
        q = pitlane::Quote{};
        q.name = 1;
        q.exch_us = static_cast<int64_t>(i) * 1000000;
        q.local_us = q.exch_us;
        q.bid_px = px - 500;
        q.ask_px = px + 500;
        q.last_px = px;
        q.bid_qty = 100;
        q.ask_qty = 100;
        q.last_qty = 10;
    }
}

static void check_backtest() {
    pitlane::Quote tape[kTapeN];
    make_tape(tape, kTapeN);

    pitlane::BtCfg cfg{};
    pitlane::BtMetrics m{};
    pitlane::Backtester<1024> bt;
    expect(bt.run(tape, kTapeN, cfg, m), "bt run");
    expect(m.trades > 0, "bt trades");
    expect(m.points == bt.curve_points() && m.points >= 2, "bt points");
    expect(m.fees >= 0 && m.max_dd_ppm >= 0, "bt fees and drawdown");
    expect(m.win_ppm >= 0 && m.win_ppm <= 1000000, "bt win range");
    expect(m.net_pnl == m.final_equity - cfg.cash, "bt net pnl");

    pitlane::BtMetrics again{};
    expect(bt.run(tape, kTapeN, cfg, again) && again.net_pnl == m.net_pnl, "bt determinism");
    expect(again.trades == m.trades && again.final_equity == m.final_equity, "bt determinism fields");
}

static void check_factor_lab() {
    pitlane::Quote tape[kTapeN];
    make_tape(tape, kTapeN);

    pitlane::LabRow rows[pitlane::FactorLab::kMaxRows]{};
    pitlane::FactorLab lab;
    const int nr = lab.sweep(tape, kTapeN, 1, 0, 420, rows, pitlane::FactorLab::kMaxRows);
    expect(nr > 0, "lab sweep rows");

    pitlane::FactorLab::sort(rows, nr);
    bool sorted = true;
    for (int i = 1; i < nr; ++i) {
        if (rows[i - 1].score_ppm < rows[i].score_ppm) sorted = false;
    }
    expect(sorted, "lab score descending");

    // Constant-size quotes make the order-flow factors (OFI/imbalance) identically
    // zero; ties still rank them highly, so pick a price factor for the trigger.
    int ri = -1;
    for (int i = 0; i < nr; ++i) {
        if (rows[i].kind == 2 && rows[i].turnover_ppm > 0) {
            ri = i;
            break;
        }
    }
    expect(ri >= 0, "lab momentum row");

    pitlane::LabPick pick{};
    const bool picked = ri >= 0 && lab.pick(tape, kTapeN, rows[ri < 0 ? 0 : ri], 1, 420, pick);
    expect(picked, "lab pick");
    expect(pick.threshold_ppm >= 1 && pick.ready > 0 && pick.trigger > 0, "lab pick fields");
}

static void check_block_tape() {
    constexpr int kN = 100000;
    constexpr int kSplit = 60000;

    std::vector<pitlane::TapeTick> ticks(kN);
    for (int i = 0; i < kN; ++i) {
        const int64_t trend = i < kN / 2 ? i : kN - i;
        const int64_t px = 1000000 + trend;
        pitlane::TapeTick& t = ticks[i];
        t.exch_us = 1000000 + static_cast<int64_t>(i) * 1000;
        t.local_us = t.exch_us + 5;
        t.bid_px = px - 100;
        t.ask_px = px + 100;
        t.last_px = px;
        t.bid_qty = 10 + i % 5;
        t.ask_qty = 20 + i % 3;
        t.last_qty = 1 + i % 7;
    }

    char path[128];
    char partial[128];
    std::snprintf(path, sizeof path, "/tmp/pitlane_study_tape_%ld.plt", static_cast<long>(::getpid()));
    std::snprintf(partial, sizeof partial, "/tmp/pitlane_study_partial_%ld.plt", static_cast<long>(::getpid()));

    {
        pitlane::BlockTapeWriter w;
        expect(w.open(path, 3), "tape open");
        expect(w.append(ticks.data(), kSplit) && w.commit(), "tape version 1");
        expect(w.append(ticks.data() + kSplit, kN - kSplit) && w.commit(), "tape version 2");
    }

    pitlane::BlockTapeReader r;
    expect(r.open(path), "tape reopen");
    pitlane::TapeMeta meta{};
    expect(r.meta(meta), "tape meta");
    expect(meta.versions == 2, "tape versions");
    expect(meta.rows == static_cast<uint64_t>(kN), "tape rows");
    // Every commit flushes its pending partial block, so the 60k/40k split costs
    // one more block than ceil(100000 / 1024) would for a single commit.
    expect(meta.blocks == 99, "tape blocks");
    expect(r.rows_of(1) == 60000 && r.rows_of(2) == 100000, "tape rows_of");

    pitlane::TapeTick out[256];
    uint32_t n = 0;
    r.reset_blocks_read();
    expect(r.rows(0, INT64_MIN, INT64_MAX, 0, 10, pitlane::kTapeColAll, out, 10, n) && n == 10, "tape first ten");
    expect(r.blocks_read() <= 2, "tape first ten touches one block");

    const int64_t mid_lo = ticks[50000].exch_us;
    const int64_t mid_hi = ticks[50100].exch_us;
    expect(r.rows(0, mid_lo, mid_hi, 0, 0, pitlane::kTapeColAll, out, 256, n) && n == 101, "tape range count");
    expect(out[0].exch_us == mid_lo && out[n - 1].exch_us == mid_hi, "tape range bounds");

    for (pitlane::TapeTick& t : out) t = pitlane::TapeTick{};
    expect(r.rows(0, INT64_MIN, INT64_MAX, 0, 4, pitlane::kTapeColBid, out, 4, n) && n == 4, "tape project bid");
    for (uint32_t i = 0; i < n; ++i) {
        expect(out[i].bid_px == ticks[i].bid_px, "tape projected bid value");
        expect(out[i].ask_px == 0 && out[i].exch_us == 0, "tape unprojected columns untouched");
    }

    pitlane::TapeStats st{};
    expect(r.describe(0, st), "tape describe");
    expect(st.version == 2 && st.rows == static_cast<uint64_t>(kN), "tape describe rows");
    int64_t bid_min = ticks[0].bid_px;
    int64_t bid_max = ticks[0].bid_px;
    double bid_sum = 0;
    for (int i = 0; i < kN; ++i) {
        const int64_t v = ticks[i].bid_px;
        if (v < bid_min) bid_min = v;
        if (v > bid_max) bid_max = v;
        bid_sum += static_cast<double>(v);
    }
    const double bid_mean = bid_sum / static_cast<double>(kN);
    expect(st.col[2].min == bid_min && st.col[2].max == bid_max, "tape describe bid range");
    expect(std::fabs(st.col[2].mean - bid_mean) <= 1e-6 * std::fabs(bid_mean), "tape describe bid mean");

    std::vector<pitlane::Quote> quotes(kN);
    uint32_t got = 0;
    expect(r.load_quotes(2, quotes.data(), kN, got) && got == static_cast<uint32_t>(kN), "tape load quotes");
    const int spots[] = {0, 1, 1022, 1023, 1024, 1025, 59999, 60000, 60001, 99999};
    for (int idx : spots) {
        const pitlane::Quote& q = quotes[idx];
        expect(q.exch_us == ticks[idx].exch_us && q.bid_px == ticks[idx].bid_px &&
                   q.ask_px == ticks[idx].ask_px && q.last_qty == ticks[idx].last_qty,
               "tape quote spot check");
    }
    expect(quotes[0].name == 3, "tape quote symbol");

    {
        pitlane::BlockTapeWriter w;
        expect(w.open(partial, 3), "partial tape open");
        expect(w.append(ticks.data(), 5000), "partial tape append");
        w.close(); // no commit
    }
    pitlane::BlockTapeReader uncommitted;
    expect(!uncommitted.open(partial), "tape without commit is unreadable");

    struct stat sb {};
    expect(::stat(path, &sb) == 0, "tape stat");
    expect(::truncate(path, sb.st_size - 32) == 0, "tape truncate");
    pitlane::BlockTapeReader truncated;
    expect(!truncated.open(path), "truncated tape is rejected");

    ::unlink(path);
    ::unlink(partial);
}

// 夜盘跨 00:00 时 exch_us 会回退，块式 tape 应接成连续会话时钟。
static void check_block_tape_night() {
    constexpr int64_t kDayUs = 86400ll * 1000000;
    const int64_t raw[4] = {75600000000ll, 86399000000ll, 1000000ll, 9000000000ll};
    pitlane::TapeTick ticks[4]{};
    for (int i = 0; i < 4; ++i) {
        ticks[i].exch_us = raw[i];
        ticks[i].local_us = 1700000000000000ll + static_cast<int64_t>(i);
        ticks[i].bid_px = 100 + i;
        ticks[i].ask_px = 101 + i;
        ticks[i].last_px = 100 + i;
        ticks[i].bid_qty = 1;
        ticks[i].ask_qty = 1;
    }
    char path[128];
    std::snprintf(path, sizeof path, "/tmp/pitlane_study_night_%ld.plt", static_cast<long>(::getpid()));
    {
        pitlane::BlockTapeWriter w;
        expect(w.open(path, 3), "night tape open");
        expect(w.append(ticks, 4) && w.commit(), "night tape commit");
    }
    pitlane::BlockTapeReader r;
    expect(r.open(path), "night tape reopen");
    pitlane::TapeMeta meta{};
    expect(r.meta(meta), "night tape meta");
    expect(meta.first_ts == raw[0] && meta.last_ts == kDayUs + raw[3], "night tape meta range");

    pitlane::TapeTick out[8]{};
    uint32_t n = 0;
    expect(r.rows(0, INT64_MIN, INT64_MAX, 0, 8, pitlane::kTapeColAll, out, 8, n) && n == 4, "night tape rows");
    expect(out[2].exch_us == kDayUs + raw[2] && out[3].exch_us == kDayUs + raw[3], "night tape rollover");
    expect(r.rows(0, kDayUs, INT64_MAX, 0, 8, pitlane::kTapeColAll, out, 8, n) && n == 2, "night tape after midnight");
    expect(r.rows(0, raw[0], raw[1], 0, 8, pitlane::kTapeColAll, out, 8, n) && n == 2, "night tape before midnight");
    ::unlink(path);
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
    check_calendar();
    check_faces();
    check_shadow();
    check_perf();
    check_backtest();
    check_factor_lab();
    check_block_tape();
    check_block_tape_night();
    if (g_fails) {
        std::fprintf(stderr, "%d study check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
