#include "pitlane/pitlane.hpp"

#include <cstdio>

// One order from the checks, through the sheet, with the queue and the lot split beside it.

int fail(const char* what) {
    std::fprintf(stderr, "order: %s\n", what);
    return 1;
}

int main() {
    pitlane::TimeLine<8> line;
    uint64_t front = 0;
    if (!line.push(11) || !line.push(12) || !line.erase(11) || !line.pop(front) || front != 12) return fail("line");

    pitlane::SlotPool<int64_t, 8> pool;
    if (!pool.take(front, 100) || !pool.find(front) || *pool.find(front) != 100) return fail("pool");

    pitlane::LotSlice slices[4]{};
    const int ns = pitlane::split_lots(10, pitlane::LotPos{6, 2}, pitlane::LotPend{0, 0}, 3, 1,
                                       pitlane::SplitMode::TodayThenYdayThenOpen, slices, 4);
    if (ns <= 0) return fail("lots");

    const int64_t margin = pitlane::margin_both(100, 2, 10, 1, 10, 50);
    if (margin != 200) return fail("margin");

    pitlane::NetHold hold{};
    hold.long_today = 1;
    const pitlane::Move move = pitlane::one_move(3, hold, 100, 101, true);
    if (move.qty <= 0 || move.kind == pitlane::MoveKind::None) return fail("move");

    pitlane::GateAsk ask{};
    ask.mode = pitlane::PxMode::Limit;
    ask.off = pitlane::Off::Open;
    ask.px = 101;
    ask.qty = 1;
    ask.side = 1;
    ask.last = 100;
    ask.dev_den = 100;
    ask.dev_cap = 50;
    if (pitlane::gate_ask(ask) != pitlane::Cause::Ok) return fail("gate");

    pitlane::Sheet sheet{};
    if (!pitlane::sheet_open(sheet, 8, 1, 101, 1)) return fail("open");
    if (!pitlane::sheet_apply(sheet, pitlane::SheetOp::Stage, 0, 1000, pitlane::Cause::Ok)) return fail("stage");
    if (!pitlane::sheet_apply(sheet, pitlane::SheetOp::Accept, 0, 1100, pitlane::Cause::Ok)) return fail("accept");
    if (!pitlane::sheet_apply(sheet, pitlane::SheetOp::Fill, 1, 1200, pitlane::Cause::Ok)) return fail("fill");
    if (sheet.phase != pitlane::Phase::Partial || sheet.filled != 1) return fail("partial");
    if (pitlane::life_next(pitlane::Phase::Partial, pitlane::LifeEvent::Finish) != pitlane::Phase::Filled) return fail("life");

    pitlane::TextBuf<160> text;
    if (!pitlane::sheet_line(sheet, text)) return fail("line text");
    std::printf("order slices=%d margin=%lld %s\n", ns, static_cast<long long>(margin), text.c_str());
    return 0;
}
