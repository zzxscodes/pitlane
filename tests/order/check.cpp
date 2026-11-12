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

static bool wake_until(int index, void* user) {
    auto* n = static_cast<int*>(user);
    if (index >= *n) return false;
    return true;
}

static void check_lots() {
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
}

static void check_moves() {
    pitlane::NetHold hold{1, 2, 0, 0};
    const pitlane::Move flat = pitlane::one_move(0, hold, 10, 11, true);
    expect(flat.kind == pitlane::MoveKind::SellCloseYday && flat.qty == 2 && flat.px == 10, "close yesterday first");
    const pitlane::Move grow = pitlane::one_move(5, hold, 10, 11, true);
    expect(grow.kind == pitlane::MoveKind::BuyOpen && grow.qty == 2 && grow.px == 11, "open the gap");

    pitlane::LinearBook lin{2, 0, 0, 0};
    expect(lin.notion(10, 3) == 60, "linear notion");
    pitlane::apply_fill(lin, true, 10, 3, 1);
    expect(lin.pos == 3 && lin.cash == -60 && lin.equity(10) == -1, "long equity");
}

static void check_life() {
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

void check_pool() {
    pitlane::SlotPool<int, 8> pool;
    expect(pool.take(3, 30) && pool.take(9, 90) && pool.size() == 2, "take");
    expect(!pool.take(3, 1) && pool.find(3) && *pool.find(3) == 30, "dup");
    expect(pool.drop(3) && !pool.find(3) && pool.take(3, 31) && *pool.find(3) == 31, "reuse");
    expect(pool.find(9) && *pool.find(9) == 90, "other id");
    for (uint64_t id = 10; id < 16; ++id) expect(pool.take(id, static_cast<int>(id)), "fill");
    expect(!pool.take(20, 1) && pool.size() == 8, "full");
    expect(!pool.take(0, 1), "zero id");
}

static void check_line() {
    pitlane::TimeLine<4> line;
    uint64_t id = 0;
    expect(line.push(1) && line.push(2) && line.push(3) && line.front(id) && id == 1, "line");
    expect(!line.push(2) && line.erase(2) && line.pop(id) && id == 1 && line.pop(id) && id == 3, "erase");
    expect(line.push(4) && line.push(5) && line.push(6) && line.push(7) && !line.push(8), "line full");
}

static void check_margin() {
    expect(pitlane::margin_both(100, 2, 10, 1, 10, 50) == 200, "money larger");
    expect(pitlane::margin_both(100, 2, 10, 1, 10, 200) == 400, "lot larger");
    expect(pitlane::margin_both(100, 2, 10, 0, 10, 0) == 0, "zero charge");
    expect(pitlane::margin_both(100, 2, 10, 1, 0, 1) == 0, "bad den");
}

static void check_shed() {
    pitlane::StayShed shed(5, 10);
    expect(!shed.drop(0, 6) && !shed.drop(0, 10), "window has not closed");
    expect(shed.drop(0, 16) && shed.dropping(), "sustained lateness");
    expect(!shed.drop(20, 20) && !shed.dropping(), "recovered");
}

static void check_ticket() {
    pitlane::Ticket ticket;
    ticket.tag = 9;
    ticket.phase = pitlane::Phase::Working;
    ticket.qty = 10;
    ticket.px = 100;
    pitlane::TicketMove move{};
    expect(pitlane::ticket_pull(ticket, move) && ticket.phase == pitlane::Phase::PullWait, "pull");
    expect(pitlane::ticket_fill(ticket, 2) && ticket.phase == pitlane::Phase::PullWait, "fill does not finish a pull");
    expect(!pitlane::ticket_rewind(ticket, move), "a newer fill blocks rewind");

    pitlane::Ticket fresh = ticket;
    fresh.phase = pitlane::Phase::Working;
    fresh.filled = 0;
    fresh.rev = 0;
    expect(pitlane::ticket_amend(fresh, 110, 8, move) && fresh.px == 110, "amend");
    expect(pitlane::ticket_rewind(fresh, move) && fresh.px == 100 && fresh.qty == 10, "rewind");
}

void check_parent_recon() {
    pitlane::SlicePlan plan{};
    expect(pitlane::slice_open(plan, 10, 9, 3, 1, 100), "open slices");
    pitlane::SliceCut cut{};
    expect(pitlane::slice_poll(plan, 100, cut) && cut.qty == 3 && !cut.last, "first slice");
    expect(!pitlane::slice_poll(plan, 101, cut), "too early");
    expect(pitlane::slice_poll(plan, 103, cut) && cut.qty == 3, "second slice");
    expect(pitlane::slice_poll(plan, 106, cut) && cut.qty == 4 && cut.last, "last slice keeps the remainder");
    pitlane::SlicePlan again{};
    expect(pitlane::slice_open(again, 10, 9, 3, 1, 100), "open again");
    expect(pitlane::slice_poll(again, 100, cut) && pitlane::slice_poll(again, 103, cut), "two children");
    expect(pitlane::slice_restore(again, 2) && again.pool == 6 && again.scheduled == 4, "return remainder");
    expect(pitlane::slice_late(again, 8) == 2 && again.scheduled == 2 && again.pool == 0, "late fill nets the next child");

    pitlane::BasketNote note{};
    expect(pitlane::basket_make(3, pitlane::BasketOp::Pause, "grid", 20, 64, note) && note.bytes == 20, "basket header");
    expect(!pitlane::basket_make(3, pitlane::BasketOp::Pause, "grid", 80, 64, note), "payload stays outside");

    pitlane::BookOrder order{};
    order.tag = 4;
    pitlane::SeenIds<64> seen;
    pitlane::ReconEvent ev[4]{};
    expect(pitlane::recon_fill(order, seen, 11, 4, 100, 10, ev, 4) == 2, "create from a fill");
    expect(ev[0].kind == pitlane::ReconKind::Ack && ev[1].kind == pitlane::ReconKind::Fill, "ack then fill");
    expect(pitlane::recon_fill(order, seen, 11, 4, 100, 10, ev, 4) == 1 && ev[0].kind == pitlane::ReconKind::Dup, "same fill");
    expect(pitlane::recon_fill(order, seen, 12, 20, 100, 10, ev, 4) == 2, "rest plus extra");
    expect(ev[0].kind == pitlane::ReconKind::Fill && ev[0].qty == 6 && ev[1].kind == pitlane::ReconKind::Over && order.filled == 10,
           "overfill is separate");
}

static void check_flight() {
    pitlane::FlightQueue<4> queue;
    expect(queue.push(pitlane::Flight{1, 1, 10, pitlane::FlightAct::Enter}), "enter");
    expect(queue.push(pitlane::Flight{2, 2, 5, pitlane::FlightAct::Pull}), "other name");
    pitlane::Flight out[4]{};
    expect(queue.take(1, 10, out, 4) == 1 && out[0].tag == 1 && queue.size() == 1, "other name stays");
    expect(queue.take(2, 4, out, 4) == 0 && queue.take(2, 5, out, 4) == 1, "not due yet");
}

void check_sign_ack() {
    pitlane::SignSlip slip;
    slip.need = 0;
    expect(pitlane::sign_ready(slip), "no signer required");
    slip.need = 2;
    expect(pitlane::sign_pass(slip) && !pitlane::sign_ready(slip) && pitlane::sign_pass(slip) && pitlane::sign_ready(slip), "two passes");
    expect(pitlane::sign_reject(slip) && !pitlane::sign_pass(slip), "reject sticks");
    pitlane::sign_reset(slip);
    expect(pitlane::sign_pass(slip) && !pitlane::sign_ready(slip), "need remains");

    pitlane::OpenQuota quota{10, 2};
    pitlane::OpenHold hold{};
    expect(pitlane::hold_reserve(quota, hold, true, 4) && quota.lots == 6 && quota.cancels == 1, "open holds a cancel");
    expect(pitlane::hold_cancel(quota, hold, 1, false) && quota.lots == 9 && quota.cancels == 1 && !hold.on, "cancel reached the venue");
    expect(pitlane::hold_reserve(quota, hold, true, 4) && quota.lots == 5 && quota.cancels == 0, "second open");
    expect(pitlane::hold_done(quota, hold, 4, true) && quota.cancels == 0 && !hold.on, "outside fill keeps the credit");
    expect(!pitlane::hold_reserve(quota, hold, true, 4), "no cancel credit left");
    quota.cancels = 1;
    expect(pitlane::hold_reserve(quota, hold, true, 4) && quota.lots == 1 && quota.cancels == 0, "third open");
    expect(pitlane::hold_done(quota, hold, 4, false) && quota.cancels == 1, "full fill returns the credit");
}

void check_sign_rest() {
    pitlane::OpenQuota quota{10, 2};
    pitlane::OpenHold hold{};
    expect(pitlane::hold_reserve(quota, hold, true, 4), "reserve");
    expect(pitlane::hold_cancel(quota, hold, 0, true) && quota.lots == 10 && quota.cancels == 2 && !hold.on, "reject never sent");
    expect(pitlane::hold_reserve(quota, hold, false, 3) && quota.lots == 10 && quota.cancels == 2, "close holds nothing");
    expect(pitlane::hold_late(quota, hold, 0) && quota.cancels == 2 && !hold.on, "close timeout");
    expect(pitlane::hold_reserve(quota, hold, true, 4) && quota.lots == 6 && quota.cancels == 1, "open again");
    expect(pitlane::hold_cancel_late(quota, hold) && hold.on && quota.cancels == 2 && !hold.cancel_held, "cancel timeout");
    expect(pitlane::hold_done(quota, hold, 4, false) && quota.cancels == 2, "later fill does not return it twice");
    expect(pitlane::hold_reserve(quota, hold, true, 2), "last open");
    expect(pitlane::hold_late(quota, hold, 0) && quota.lots == 6 && quota.cancels == 2, "open timeout returns both");

    pitlane::AckBook<2> acks;
    pitlane::VenueKey num{};
    pitlane::VenueKey text{};
    expect(pitlane::key_num(num, 9, "ctp") && pitlane::key_text(text, "ABC", "ctp") && !pitlane::key_same(num, text), "different key kinds");
    expect(!pitlane::key_text(num, "this name is longer than the field", "ctp") && num.num == 9, "long text keeps the old key");
    expect(acks.put(num, 11, 0) && acks.put(text, 12, 0) && !acks.put(num, 13, 1), "full book");
    pitlane::AckClock clock;
    clock.place_lim = 10;
    clock.cancel_lim = 5;
    clock.gap = 100;
    uint64_t ids[4]{};
    expect(acks.sweep(clock, 10, ids, 4) == 0, "not due yet");
    expect(acks.sweep(clock, 11, ids, 4) == 0, "inside the gap");
    expect(acks.sweep(clock, 111, ids, 4) == 2, "both place timers");
    expect(acks.link(num) && acks.arm_cancel(num, 111), "cancel armed");
    expect(acks.sweep(clock, 111, ids, 4) == 0, "gap after the sweep");
    expect(acks.sweep(clock, 220, ids, 4) == 2, "place and cancel");
    expect(!acks.link(num), "row was not cleared");
}

static void check_typed() {
    auto draft = pitlane::ticket_draft(9, 10);
    auto live = pitlane::go_live(pitlane::mark_ready(draft));
    live = pitlane::apply_fill(live, 3);
    expect(pitlane::ticket_id(live) == 9 && pitlane::ticket_leaves(live) == 7, "typed live");
    auto done = pitlane::seal(live);
    expect(pitlane::ticket_leaves(done) == 0, "typed done");
}

static void check_latch() {
    pitlane::PaceLatch latch;
    pitlane::latch_cross(latch, 100, 50, 0, 0);
    expect(latch.cross_on == 1, "cross on");
    pitlane::latch_cross(latch, 100, 2, 0, 0);
    expect(latch.cross_on == 0, "cross off");
    pitlane::latch_trim(latch, 100, 20, 0);
    expect(latch.trim_on == 1, "trim on");
    expect(pitlane::window_back(1000000, 30000, 100000) == 100000, "window");
}

static void check_idclaim() {
    pitlane::IdClaim<4> ids;
    expect(ids.claim(10, 1) && ids.arm_live(10), "claim live");
    pitlane::IdKind st = pitlane::IdKind::Empty;
    expect(ids.resubmit(10, st) == 0 && st == pitlane::IdKind::Live, "live dup");
    expect(ids.seal(10) && ids.resubmit(10, st) == 1 && st == pitlane::IdKind::Final, "final");
    expect(ids.claim(11, 1) && ids.drop_wait(11) && ids.find(11) < 0, "drop wait");
    expect(!ids.claim(0, 1), "zero id");
}

static void check_basket_lane_inflight() {
    pitlane::BasketBook<4> book;
    expect(book.open(9) && book.put(1, 100) && book.put(2, 50), "basket open");
    expect(pitlane::basket_gap(*book.at(0)) == 100, "gap");
    expect(book.add_working(1, 30) && pitlane::basket_topup(*book.at(0), 10) == 70, "topup");
    expect(book.add_done(1, 10) && pitlane::basket_gap(*book.at(0)) == 70, "done");

    pitlane::LaneBook<4> lanes;
    pitlane::LaneRule rule{};
    rule.limit = 1;
    rule.market = 1;
    expect(lanes.put(7, rule) && pitlane::lane_pick(lanes, 7, rule) && !lanes.pick(8), "lane book");

    pitlane::BrokerWatch watch;
    pitlane::Intent in{};
    in.tag = 42;
    in.side = 1;
    in.qty = 1;
    in.px = 10;
    expect(watch.remember(in, 1000), "remember");
    uint64_t tags[4]{};
    expect(watch.due(2000, 500, 0, 0, tags, 4) == 1 && tags[0] == 42 && watch.live() == 1, "due keeps");
    pitlane::IdClaim<4> claim;
    expect(claim.claim(42, 1) && claim.arm_live(42), "claim");
    pitlane::InflightScan opt{};
    opt.now_us = 2000;
    opt.ack_limit_us = 500;
    opt.claim_all_live = 1;
    expect(pitlane::inflight_scan(watch, &claim, static_cast<pitlane::AckBook<4>*>(nullptr), opt, tags, 4) == 1,
           "inflight");
}

static void check_gate_cash_risk() {
    pitlane::GateAsk ask{};
    ask.act = pitlane::Act::Enter;
    ask.mode = pitlane::PxMode::Limit;
    ask.px = 10;
    ask.qty = 5;
    ask.side = 1;
    pitlane::CashLine cash{};
    cash.free_amt = 30;
    ask.cash = &cash;
    ask.cash_need = 40;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::NoCash, "gate cash");
    ask.cash_need = 20;
    pitlane::RiskCaps caps{};
    caps.max_qty = 3;
    ask.risk = &caps;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::BadQty, "gate risk qty");
    caps.max_qty = 10;
    ask.qty = 5;
    pitlane::LotLock lots{};
    lots.total = 2;
    lots.free_lots = 2;
    ask.lots = &lots;
    ask.lock_qty = 3;
    ask.off = pitlane::Off::Close;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::NoPos, "gate pos");
    ask.lock_qty = 0;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::Ok, "gate ok");
}

static void check_lots_margin_sign_pull() {
    expect(pitlane::board_lot_ok(200) && !pitlane::board_lot_ok(150) && pitlane::board_lot_floor(250) == 200, "board lot");
    pitlane::MarginBook m{};
    m.buying_power = 10000;
    m.short_avail = 5;
    expect(pitlane::margin_buy_ok(m, 100, 50) && !pitlane::margin_buy_ok(m, 100, 200), "margin buy");
    expect(pitlane::margin_short_ok(m, 5) && !pitlane::margin_short_ok(m, 6), "margin short");

    pitlane::SignSlip slip{};
    expect(pitlane::sign_begin(slip, 3) && !pitlane::sign_ready(slip), "sign begin");
    expect(pitlane::sign_pass(slip) && pitlane::sign_pass(slip) && pitlane::sign_pass(slip) && pitlane::sign_ready(slip),
           "sign three");

    pitlane::Sheet sheet{};
    expect(pitlane::sheet_open(sheet, 1, 1, 100, 10), "sheet");
    expect(pitlane::sheet_apply(sheet, pitlane::SheetOp::Stage, 0, 1, pitlane::Cause::Ok), "stage");
    expect(pitlane::sheet_apply(sheet, pitlane::SheetOp::Accept, 0, 2, pitlane::Cause::Ok), "accept");
    expect(pitlane::sheet_apply(sheet, pitlane::SheetOp::AskPull, 0, 3, pitlane::Cause::Ok), "ask pull");
    expect(pitlane::sheet_apply(sheet, pitlane::SheetOp::Fill, 3, 4, pitlane::Cause::Ok) && sheet.phase == pitlane::Phase::PullWait,
           "late fill pullwait");
    expect(pitlane::sheet_apply(sheet, pitlane::SheetOp::ConfirmPull, 4, 5, pitlane::Cause::Ok) &&
               sheet.left == 3 && sheet.phase == pitlane::Phase::Partial,
           "partial cancel");

    pitlane::BookOrder order{};
    order.known = 1;
    order.qty = 10;
    order.filled = 2;
    order.phase = pitlane::Phase::PullWait;
    pitlane::ReconEvent ev{};
    expect(pitlane::recon_cancel_qty(order, 3, ev) && order.qty == 7 && order.phase == pitlane::Phase::Partial, "recon part");
    expect(!pitlane::recon_cancel_qty(order, 20, ev), "recon over");
}

int main() {
    check_lots();
    check_ledger();
    check_moves();
    check_life();
    check_bind_lane();
    check_judge();
    check_sheet();
    check_gate();
    check_pool();
    check_line();
    check_margin();
    check_shed();
    check_ticket();
    check_parent_recon();
    check_flight();
    check_sign_ack();
    check_sign_rest();
    check_typed();
    check_latch();
    check_idclaim();
    check_basket_lane_inflight();
    check_gate_cash_risk();
    check_lots_margin_sign_pull();
    if (g_fails) {
        std::fprintf(stderr, "%d order check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
