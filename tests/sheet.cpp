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

void check_sheet() {
    pitlane::Sheet sheet;
    expect(!pitlane::sheet_open(sheet, 0, 1, 10, 4), "open id");
    expect(!pitlane::sheet_open(sheet, 7, 1, 0, 4), "open px");
    expect(pitlane::sheet_open(sheet, 7, 1, 10, 4), "open");
    expect(sheet.phase == pitlane::Phase::None && sheet.left == 4 && sheet.send_ns < 0, "opened");

    expect(pitlane::sheet_apply(sheet, pitlane::SheetOp::Stage, 0, 100, pitlane::Cause::Ok), "stage");
    expect(sheet.phase == pitlane::Phase::Staged && sheet.send_ns == 100, "staged");
    expect(pitlane::sheet_apply(sheet, pitlane::SheetOp::Stage, 0, 150, pitlane::Cause::Ok) == false, "second stage");
    expect(sheet.send_ns == 100 && sheet.phase == pitlane::Phase::Staged, "stage kept");

    expect(pitlane::sheet_apply(sheet, pitlane::SheetOp::Accept, 0, 140, pitlane::Cause::Ok), "accept");
    int64_t ack = 0;
    expect(pitlane::sheet_ack_ns(sheet, ack) && ack == 40, "ack lap");
    expect(pitlane::sheet_bind(sheet, 90), "bind");
    expect(!pitlane::sheet_bind(sheet, 91), "rebind");

    pitlane::Sheet before = sheet;
    expect(!pitlane::sheet_apply(sheet, pitlane::SheetOp::Fill, 5, 200, pitlane::Cause::Ok), "overfill");
    expect(std::memcmp(&sheet, &before, sizeof(sheet)) == 0, "overfill unchanged");
    expect(pitlane::sheet_apply(sheet, pitlane::SheetOp::Fill, 4, 200, pitlane::Cause::Ok), "fill");
    expect(sheet.phase == pitlane::Phase::Partial && sheet.filled == 4 && sheet.left == 0, "flat partial");
    expect(pitlane::sheet_apply(sheet, pitlane::SheetOp::Finish, 0, 250, pitlane::Cause::Ok), "finish");
    int64_t done = 0;
    expect(sheet.phase == pitlane::Phase::Filled && pitlane::sheet_done_ns(sheet, done) && done == 150, "done lap");

    pitlane::Sheet denied;
    expect(pitlane::sheet_open(denied, 8, 2, 11, 1), "deny open");
    expect(pitlane::sheet_apply(denied, pitlane::SheetOp::Stage, 0, 1, pitlane::Cause::Ok), "deny stage");
    expect(pitlane::sheet_apply(denied, pitlane::SheetOp::Reject, 0, 2, pitlane::Cause::DupId), "reject");
    expect(denied.phase == pitlane::Phase::Denied && denied.cause == pitlane::Cause::DupId && denied.done_ns == 2, "denied");
    expect(!pitlane::sheet_apply(denied, pitlane::SheetOp::Accept, 0, 3, pitlane::Cause::Ok), "after deny");

    pitlane::TextBuf<96> line;
    line.put("keep");
    expect(pitlane::sheet_line(denied, line), "line");
    expect(std::strstr(line.c_str(), "phase=denied") != nullptr && std::strstr(line.c_str(), "cause=dup") != nullptr, "line text");
}

void check_gate() {
    pitlane::LaneRule lane;
    lane.market = 0;
    pitlane::NameList names;
    expect(names.add("ABC"), "name");
    pitlane::FlowWindow flow;
    pitlane::WashRow row{};
    row.acct = 3;
    row.name = 9;
    row.px = 100;
    row.side = 2;
    row.live = 1;

    pitlane::GateAsk ask;
    ask.lane = &lane;
    ask.names = &names;
    ask.symbol = "ABC";
    ask.flow = &flow;
    ask.now = 10;
    ask.flow_span = 100;
    ask.flow_limit = 2;
    ask.px = 100;
    ask.qty = 1;
    ask.side = 1;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::Blocked, "listed");
    expect(flow.count(10, 100) == 0, "list skips flow");

    ask.symbol = "ZZ";
    ask.mode = pitlane::PxMode::Market;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::BadPrice, "market off");
    expect(flow.count(10, 100) == 0, "mode skips flow");

    ask.mode = pitlane::PxMode::Limit;
    ask.last = 100;
    ask.dev_den = 100;
    ask.dev_cap = 5;
    ask.px = 110;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::BadPrice, "deviation");

    ask.px = 100;
    ask.dev_cap = -1;
    ask.wash = &row;
    ask.wash_n = 1;
    ask.acct = 3;
    ask.name_id = 9;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::WashRisk, "wash");

    ask.wash_n = 0;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::Ok, "first");
    expect(flow.count(10, 100) == 1, "noted");
    expect(pitlane::gate_ask(ask) == pitlane::Cause::OrderFlow, "second");
    expect(flow.count(10, 100) == 1, "flow held");

    ask.act = pitlane::Act::Pull;
    ask.flow_limit = 0;
    lane.pull = 0;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::PullCap, "pull");
}

}  // namespace

int main() {
    check_sheet();
    check_gate();
    return g_fails == 0 ? 0 : 1;
}
