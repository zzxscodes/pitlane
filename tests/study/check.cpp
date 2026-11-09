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

static void check_right() {
    expect(pitlane::right_value(1, 110, 100) == 10 && pitlane::right_value(2, 110, 100) == 0, "call");
    expect(pitlane::right_value(2, 90, 100) == 10 && pitlane::right_value(3, 90, 100) == -1, "put");
}

int main() {
    check_feat_and_mark();
    check_tables();
    check_step();
    check_corp();
    check_right();
    if (g_fails) {
        std::fprintf(stderr, "%d study check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
