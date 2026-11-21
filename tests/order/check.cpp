#include "pitlane/pitlane.hpp"
#include "pitlane/order/ledger_feed.hpp"
#include "pitlane/order/manual.hpp"

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

static void check_ledger_view() {
    pitlane::LedgerView<8> view{};
    pitlane::LedgerAsset a{};
    a.tier = static_cast<uint8_t>(pitlane::LedgerTier::Strategy);
    a.counter = 1;
    a.acct = 2;
    a.trader = 3;
    a.strategy = 4;
    a.product = 5;
    a.dynamic_equity = 100;
    a.total_cash = 60;
    expect(view.upsert(a), "asset strategy");
    a.strategy = 6;
    a.dynamic_equity = 50;
    a.total_cash = 30;
    expect(view.upsert(a), "asset second strategy");

    a.dynamic_equity = 70;
    a.total_cash = 60;
    a.strategy = 4;
    expect(view.upsert(a) && view.assets() == 2, "asset replaces");

    pitlane::LedgerAsset parent{};
    parent.tier = static_cast<uint8_t>(pitlane::LedgerTier::Trader);
    parent.trader = 3;
    parent.dynamic_equity = 170;
    expect(view.upsert(parent) && view.assets() == 3, "asset trader");

    pitlane::LedgerFilter f{};
    f.tier = static_cast<uint8_t>(pitlane::LedgerTier::Strategy);
    f.trader = 3;
    pitlane::LedgerAsset page[2]{};
    int next = 0;
    bool last = false;
    expect(view.query_assets(f, 0, page, 1, &next, &last) == 1 && page[0].strategy == 4 &&
               page[0].last == 0 && !last,
           "ledger page one");
    expect(view.query_assets(f, next, page, 1, &next, &last) == 1 && page[0].strategy == 6 &&
               page[0].last == 1 && last,
           "ledger page two");
    const pitlane::LedgerAsset sum = view.sum_assets(f);
    expect(sum.dynamic_equity == 120 && sum.total_cash == 90, "ledger sum");

    pitlane::LedgerPosition p{};
    p.tier = static_cast<uint8_t>(pitlane::LedgerTier::Strategy);
    p.counter = 1;
    p.acct = 2;
    p.trader = 3;
    p.strategy = 4;
    p.product = 5;
    p.name = 9;
    p.direction = static_cast<uint8_t>(pitlane::LedgerSide::Long);
    p.total = 10;
    p.sellable = 4;
    expect(view.upsert(p), "position long");
    p.direction = static_cast<uint8_t>(pitlane::LedgerSide::Short);
    p.total = 3;
    expect(view.upsert(p) && view.positions() == 2, "position short");

    pitlane::LedgerFilter pf{};
    pf.name = 9;
    pf.direction = static_cast<uint8_t>(pitlane::LedgerSide::Short);
    pitlane::LedgerPosition got[2]{};
    expect(view.query_positions(pf, 0, got, 2, &next, &last) == 1 && got[0].total == 3 && got[0].last == 1 &&
               last,
           "position filter");

    pitlane::LedgerAsset product{};
    product.tier = static_cast<uint8_t>(pitlane::LedgerTier::Product);
    product.product = 5;
    product.counter = 99;
    product.acct = 99;
    product.trader = 99;
    product.strategy = 99;
    expect(view.upsert(product), "product normalizes");
    pitlane::LedgerFilter prod{};
    prod.tier = static_cast<uint8_t>(pitlane::LedgerTier::Product);
    expect(view.query_assets(prod, 0, page, 2, &next, &last) == 1 && page[0].counter == 0 &&
               page[0].strategy == 0,
           "product keys zeroed");
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
    expect(pitlane::cause_band(pitlane::Cause::Venue) == pitlane::CauseBand::External, "venue band");
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

    pitlane::ExtOrder ext{};
    expect(pitlane::sheet_ext(sheet, 3, 9, ext), "sheet ext");
    expect(ext.tag == 7 && ext.counter == 90 && ext.name == 3 && ext.acct == 9, "ext keys");
    expect(ext.px == 10 && ext.qty == 4 && ext.filled == 4 && ext.left == 0, "ext size");
    expect(ext.side == 1 && ext.phase == static_cast<uint8_t>(pitlane::Phase::Filled), "ext state");
    expect(ext.off == 0 && ext.hedge == 0, "ext defaults");

    pitlane::Sheet timed = sheet;
    timed.send_ns = 1500000;
    timed.ack_ns = 2000000;
    timed.done_ns = -1;
    expect(pitlane::sheet_ext(timed, 3, 9, ext), "sheet ext timed");
    expect(ext.send_us == 1500 && ext.ack_us == 2000 && ext.done_us == -1, "ext micros keep unset");
    timed.send_ns = -5;
    expect(pitlane::sheet_ext(timed, 3, 9, ext) && ext.send_us == -1, "ext negative stamp");
    pitlane::ExtOrder blank{};
    expect(!pitlane::sheet_ext(pitlane::Sheet{}, 3, 9, blank), "ext needs an order");

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

static void check_prelock_credit_curve_wash() {
    expect(pitlane::prelock_ok(1000, 400, 10, 100, 4), "prelock ok");
    pitlane::FreezeSlot slot{};
    expect(pitlane::prelock_reserve(slot, 9, 100, 4) && pitlane::prelock_commit(slot, 9), "prelock commit");
    expect(pitlane::prelock_clear(slot, 9), "prelock clear");

    pitlane::CreditCash c{};
    expect(pitlane::credit_apply_liability(c, 5, false) && c.short_qty == 5, "liab");
    expect(pitlane::credit_apply_liability(c, 3, true) && c.special_short == 3, "special");
    expect(pitlane::option_free(10, 8, 6) == 12, "option free");

    int64_t w[4] = {1, 1, 1, 1};
    pitlane::CurvePlan<4> curve{};
    expect(pitlane::curve_open(curve, 100, w, 4, 1), "curve open");
    expect(pitlane::curve_target(curve, 1) == 50, "curve target");
    const int64_t share = pitlane::lag_share_remaining(curve, 0);
    expect(share > 0 && pitlane::curve_fill(curve, share), "curve fill");

    pitlane::WashRow rows[2]{};
    rows[0].live = 1;
    rows[0].acct = 1;
    rows[0].name = 7;
    rows[0].px = 100;
    rows[0].side = 2;
    pitlane::WashHit hits[2]{};
    expect(pitlane::wash_hit(rows, 1, 1, 0, 7, 1, 100), "wash hit");
    expect(pitlane::wash_plan(rows, 1, 1, 0, 7, 1, 100, pitlane::WashAct::RejectRest, hits, 2) == 1, "wash plan");
    expect(pitlane::freeze_px(true, 0, 99, 101) == 101, "freeze ask");

    pitlane::HysteresisGate g{};
    pitlane::hyst_update(g, 20, 15, 10);
    expect(g.on == 1, "hyst on");
    pitlane::hyst_update(g, 9, 15, 10);
    expect(g.on == 0, "hyst off");
    expect(pitlane::end_flat_pull(3, 5) == 3 && pitlane::end_flat_pull(9, 5) == 0, "end flat");
}

static void check_align_220() {
    pitlane::LotSlice slices[8]{};
    const int n = pitlane::split_against(pitlane::LotPos{10, 4}, pitlane::LotPend{}, pitlane::CloseStyle::SplitYday, 12, 3,
                                          1, slices, 8);
    expect(n == 5, "against slices");
    expect(slices[0].off == pitlane::Off::CloseToday && slices[0].qty == 3, "close today cap");
    expect(slices[1].off == pitlane::Off::CloseToday && slices[1].qty == 1, "close today rest");
    expect(slices[2].off == pitlane::Off::CloseYday && slices[2].qty == 3, "close yday cap");
    expect(slices[3].off == pitlane::Off::CloseYday && slices[3].qty == 3, "close yday rest");
    expect(slices[4].off == pitlane::Off::Open && slices[4].qty == 2, "open the shortfall");
    const int plain = pitlane::split_against(pitlane::LotPos{10, 4}, {}, pitlane::CloseStyle::Plain, 12, 3, 1, slices, 8);
    expect(plain == 5 && slices[0].off == pitlane::Off::Close && slices[4].off == pitlane::Off::Open, "plain close then open");

    expect(pitlane::same_side_print(true, 5, 3) && !pitlane::same_side_print(true, 3, 5), "same side");
    expect(pitlane::print_hits(true, 100, 100, 0, false, 10, 10) && !pitlane::print_hits(true, 100, 100, 0, true, 10, 10),
           "rigid equal");
    expect(pitlane::print_hits(true, 100, 99, 0, true, 10, 10) && !pitlane::print_hits(true, 100, 99, 0, true, 10, 9),
           "through and seq");
    pitlane::ExtPrint same{};
    same.seq = 10;
    same.bid_no = 9;
    same.ask_no = 2;
    same.px = 98;
    same.qty = 3;
    pitlane::PathFill hit = pitlane::ext_match_one(true, 100, 4, 0, false, 10, same);
    expect(hit.same == 1 && hit.px == 98 && hit.qty == 3 && hit.path == pitlane::MatchPath::PassiveBid, "same print");
    pitlane::ExtPrint opp = same;
    opp.bid_no = 1;
    opp.ask_no = 8;
    opp.qty = 10;
    hit = pitlane::ext_match_one(true, 100, 4, 0, false, 10, opp);
    expect(hit.same == 0 && hit.px == 100 && hit.qty == 4 && hit.path == pitlane::MatchPath::PassiveBid, "passive at order");
    const pitlane::Rung asks[] = {{100, 2}, {104, 10}};
    hit = pitlane::match_quote(true, 105, 5, asks, 2);
    expect(hit.path == pitlane::MatchPath::ActiveBid && hit.qty == 5 && hit.px == 100, "active bid");
    expect(pitlane::match_quote(true, 90, 5, asks, 2).path == pitlane::MatchPath::None, "no cross");

    expect(pitlane::next_child_qty(1000, 200, 0, 100000, 200000, 500000, 1, false) == 0, "too far ahead");
    expect(pitlane::next_child_qty(1000, 0, 0, 100000, 200000, 1000000, 1, false) == 200, "share the lag");
    expect(pitlane::next_child_qty(1000, 0, 0, 0, 100000, 0, 100, false) == 100, "curve piece");

    pitlane::QuoteTop top{};
    top.bid[0] = 100;
    top.ask[0] = 102;
    top.bid_qty = 1;
    top.ask_qty = 1;
    expect(pitlane::price_at_quote_vwap(true, 0, top, 1) == 101, "touch vwap");
    expect(pitlane::price_at_quote_vwap(true, 1, top, 2) == 102, "ask rung tick");
    expect(pitlane::pre_px(true, 0, top, true, false) == 102, "pre opposite");
    top.ask[0] = 0;
    expect(pitlane::pre_px(true, 0, top, true, false) == 100, "pre reroute");
    top.ask[0] = 110;
    top.bid[0] = 100;
    expect(pitlane::post_px(true, 100, 110, 2, 1) == 102, "post inside");
    expect(pitlane::chasing_px(true, 0, top, 2, 1) == 112, "chase ask");

    pitlane::LotLock book{};
    book.total = 4;
    book.yday_left = 4;
    book.free_lots = 4;
    pitlane::apply_open_fill(book, 2);
    expect(book.total == 6 && book.yday_left == 4, "open grows today");
    pitlane::apply_close_fill(book, pitlane::Off::Close, pitlane::Off::Close, 1);
    expect(book.total == 5 && book.yday_left == 3, "plain close eats yesterday");
    pitlane::roll_session(book);
    expect(book.yday_left == book.total && book.total == 5, "session roll");
    const pitlane::SessionSpan spans[] = {{0, 100}, {200, 300}};
    expect(pitlane::add_session_us(90, 30, spans, 2) == 220, "skip the break");

    pitlane::GateAsk ask{};
    ask.act = pitlane::Act::Enter;
    ask.mode = pitlane::PxMode::Limit;
    ask.px = 10;
    ask.qty = 5;
    ask.side = 1;
    pitlane::LotLock lots{};
    lots.total = 10;
    lots.yday_left = 2;
    lots.free_lots = 10;
    ask.lots = &lots;
    ask.lock_qty = 5;
    ask.off = pitlane::Off::CloseYday;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::NoYday, "yday short");
    ask.off = pitlane::Off::CloseToday;
    lots.yday_left = 8;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::NoToday, "today short");
    ask.lock_qty = 0;
    pitlane::CashLine cash{};
    cash.free_amt = 50;
    ask.cash = &cash;
    ask.fee_product = pitlane::Product::Future;
    ask.fee_num = 100;
    ask.fee_den = 1000;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::NoCash, "fee freeze");
    cash.free_amt = 55;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::Ok, "fee fits");
    expect(std::strcmp(pitlane::cause_text(pitlane::Cause::NoYday), "yday") == 0, "yday text");
}

static void check_interleave_locks() {
    int paths = 0;
    for (int first = 0; first < 2; ++first) {
        int64_t total = 2;
        int64_t yday = 2;
        if (first == 0) {
            --total;
            if (yday > 0) --yday;
            --total;
            if (yday > 0) --yday;
        } else {
            total -= 2;
            yday = 0;
        }
        expect(total >= 0 && yday >= 0, "interleave non-neg");
        ++paths;
    }
    expect(paths == 2, "interleave paths");
}

static pitlane::QuoteTop pace_quote(int64_t bid, int64_t ask, int64_t bid_qty = 100, int64_t ask_qty = 100) {
    pitlane::QuoteTop q{};
    q.bid[0] = bid;
    q.ask[0] = ask;
    q.bid_qty = bid_qty;
    q.ask_qty = ask_qty;
    q.last = bid;
    return q;
}

static void check_pace_slice() {
    pitlane::SlicePacer<8> pacer;
    pitlane::SliceCfg cfg{};
    cfg.total = 100;
    cfg.horizon_us = 1000;
    cfg.interval_us = 100;
    cfg.lot = 1;
    cfg.side = static_cast<uint8_t>(pitlane::Side::Buy);
    cfg.mode = 0;
    expect(pacer.open(1, cfg), "slice open");
    expect(!pacer.open(1, cfg), "slice no reopen");
    const pitlane::QuoteTop q = pace_quote(9990, 10000);
    pitlane::PaceOrder out[2]{};
    expect(pacer.poll(0, q, out, 2) == 1, "slice first cut");
    expect(out[0].qty == 10 && out[0].px == 10000 && out[0].side == 1 && out[0].last == 0, "slice first shape");
    expect(pacer.poll(50, q, out, 2) == 0, "slice not due");
    expect(pacer.on_child(out[0].id, 4, false), "slice partial");
    expect(pacer.on_child(out[0].id, 6, true), "slice child done");
    int64_t done = 0;
    int64_t working = 0;
    expect(pacer.progress(done, working) && done == 10 && working == 0, "slice progress");
    expect(pacer.poll(100, q, out, 2) == 1, "slice second cut");
    expect(pacer.pause() && pacer.poll(200, q, out, 2) == 0, "slice paused");
    expect(pacer.resume() && pacer.poll(200, q, out, 2) == 1, "slice resumed");
    expect(pacer.stop() && !pacer.stop(), "slice stop once");
    expect(pacer.state() == pitlane::PaceState::Stopped, "slice stopped state");
    expect(!pacer.close(), "slice cannot close while working");

    pitlane::SlicePacer<8> late;
    pitlane::SliceCfg lc{};
    lc.total = 10;
    lc.horizon_us = 100;
    lc.interval_us = 33;
    lc.lot = 2;
    lc.side = static_cast<uint8_t>(pitlane::Side::Buy);
    lc.mode = 0;
    expect(late.open(2, lc), "late open");
    int64_t t = 0;
    int64_t emitted = 0;
    for (int round = 0; round < 8; ++round) {
        pitlane::PaceOrder o[2]{};
        const int n = late.poll(t, q, o, 2);
        if (n == 1) {
            emitted += o[0].qty;
            expect(o[0].qty % 2 == 0, "late lot aligned");
            expect(late.on_child(o[0].id, o[0].qty, true), "late child done");
        }
        t += 33;
    }
    expect(emitted == 10, "late end-game total");
    expect(late.close(), "late closed");
}

static void check_pace_chase() {
    pitlane::ChaseBook book{};
    book.target = 0;
    book.split_yday = false;
    book.open_cap = 10000;
    book.lng.pos = 1000;
    book.lng.today = 1000;
    pitlane::ChaseCfg cfg{};
    cfg.max_lot = 400;
    cfg.min_lot = 100;
    cfg.tick = 1;
    cfg.chase_steps = 1;
    cfg.retry_us = 1000;
    cfg.cross_ok = 0;
    pitlane::ChasePacer<8> pacer;
    expect(pacer.open(7, book, cfg), "chase open");
    const pitlane::QuoteTop q = pace_quote(9990, 10000);
    pitlane::PaceOrder out[2]{};
    expect(pacer.tick(0, q, out, 2) == 1, "chase emit");
    expect(out[0].side == static_cast<uint8_t>(pitlane::Side::Sell) && out[0].qty == 400, "chase clip");
    expect(out[0].px == 9999, "chase passive inside spread");
    expect(out[0].off == static_cast<uint8_t>(pitlane::Off::Close), "chase close offset");
    const uint64_t first = out[0].id;
    expect(pacer.on_child(first, 400, true), "chase fill");
    expect(pacer.net() == 600 && !pacer.at_target(), "chase progress");
    expect(pacer.tick(1, q, out, 2) == 0, "chase retry gate");
    expect(pacer.tick(1000, q, out, 2) == 1, "chase resend");
    expect(pacer.on_reject(out[0].id), "chase reject");
    expect(pacer.stop() && !pacer.stop(), "chase stop once");
    expect(pacer.on_child(first, 1, true) == false, "chase dead child ignored");

    pitlane::ChaseBook flat{};
    flat.target = 0;
    pitlane::ChasePacer<8> done;
    expect(done.open(8, flat, cfg), "chase flat open");
    expect(done.tick(0, q, out, 2) == 0, "chase flat no move");
    expect(done.state() == pitlane::PaceState::Done, "chase flat done");

    pitlane::ChaseBook capped{};
    capped.target = 100;
    capped.open_cap = 0;
    pitlane::ChasePacer<8> stuck;
    expect(stuck.open(9, capped, cfg), "chase capped open");
    expect(stuck.tick(0, q, out, 2) == 0, "chase capped no move");
    expect(stuck.state() != pitlane::PaceState::Done && !stuck.at_target(), "chase capped waiting");
}

static void check_pace_size() {
    pitlane::SizeCfg cfg{};
    cfg.total = 500;
    cfg.threshold = 300;
    cfg.lot = 100;
    cfg.max_child = 200;
    cfg.side = static_cast<uint8_t>(pitlane::Side::Buy);
    cfg.mode = 0;
    pitlane::SizePacer<8> pacer;
    expect(pacer.open(11, cfg), "size open");
    const pitlane::QuoteTop big = pace_quote(9990, 10000, 100, 500);
    pitlane::PaceOrder out[2]{};
    expect(pacer.poll(big, out, 2) == 1 && out[0].qty == 200 && out[0].px == 10000, "size take big");
    expect(pacer.on_child(out[0].id, 200, true), "size child one");
    expect(pacer.poll(big, out, 2) == 1 && out[0].qty == 200, "size child two");
    expect(pacer.on_child(out[0].id, 200, true), "size child two fill");
    expect(pacer.poll(big, out, 2) == 1 && out[0].qty == 100, "size final clip");
    expect(pacer.on_child(out[0].id, 100, true), "size final fill");
    expect(pacer.state() == pitlane::PaceState::Done, "size done");
    expect(!pacer.open(0, cfg), "size rejects zero id");

    pitlane::SizeCfg join{};
    join.total = 100;
    join.threshold = 500;
    join.lot = 1;
    join.side = static_cast<uint8_t>(pitlane::Side::Buy);
    join.mode = 0;
    pitlane::SizePacer<8> maker;
    expect(maker.open(12, join), "size join open");
    expect(maker.poll(pace_quote(9990, 10000, 10, 10), out, 2) == 1 && out[0].px == 9990, "size joins own top");
    expect(maker.on_reject(out[0].id), "size reject");
    expect(maker.stop() && !maker.stop(), "size stop once");
}

static void check_pace_book() {
    pitlane::PaceBook<2, 8> book;
    pitlane::SliceCfg sc{};
    sc.total = 100;
    sc.horizon_us = 1000;
    sc.interval_us = 100;
    sc.lot = 1;
    sc.side = static_cast<uint8_t>(pitlane::Side::Buy);
    pitlane::SizeCfg zc{};
    zc.total = 100;
    zc.lot = 1;
    zc.max_child = 100;
    zc.side = static_cast<uint8_t>(pitlane::Side::Buy);
    expect(book.add_slice(1, sc) != nullptr, "book add slice");
    expect(book.add_size(2, zc) != nullptr, "book add size");
    expect(book.add_slice(1, sc) == nullptr, "book duplicate");
    expect(book.add_size(3, zc) == nullptr, "book full");
    expect(book.count() == 2, "book count");
    expect(book.kind(1) == pitlane::PaceBook<2, 8>::Kind::Slice, "book kind slice");
    expect(book.kind(2) == pitlane::PaceBook<2, 8>::Kind::Size, "book kind size");
    expect(book.pause(1) && book.resume(1), "book pause resume");
    const pitlane::QuoteTop q = pace_quote(9990, 10000);
    pitlane::PaceOrder out[4]{};
    const int n = book.tick_all(0, q, out, 4);
    expect(n == 2, "book tick all");
    expect(book.stop(1) && !book.stop(1), "book stop once");
    expect(book.retire(1), "book retire stopped");
    expect(!book.retire(2), "book cannot retire working");
    expect(book.count() == 1, "book retired count");
}

static void check_flow_gate() {
    pitlane::FlowGate g{};
    g.reset();
    const int64_t t = 1500000;
    g.on_order(3, 1, 100, t);
    g.on_order(3, 1, 250, t + 1000);
    expect(g.orders(3, 1, t) == 2, "flow orders 1s");
    expect(g.notional(3, 10, t) == 350, "flow notional");
    g.on_cancel(3, 1, 50, t + 2000);
    expect(g.cancels(3, 1, t + 2000) == 1, "flow cancels");
    expect(g.canceled(3, 10, t + 2000) == 50, "flow canceled notional");
    expect(g.market_orders(1, 1, t) == 2, "flow market orders");
    expect(g.lifetime_orders(3) == 2 && g.lifetime_cancels(3) == 1, "flow lifetime");
    expect(g.cancel_ratio_ppm(3) == 500000, "flow cancel ratio");
    expect(g.orders(3, 1, t + 11000000) == 0, "flow window slides");
    expect(g.lifetime_orders(3) == 2, "flow lifetime keeps");
    expect(g.orders(99, 1, t) == 0, "flow bad account");
}

static void check_precheck() {
    pitlane::PreTradeGuard g{};
    pitlane::PreLimits l{};
    l.max_notional[0] = 1000;
    l.max_orders_1s = 2;
    l.self_trade_block = 1;
    g.set_limits(l);
    g.reset();
    pitlane::PreReq r{};
    r.acct = 1;
    r.name = 5;
    r.market = 0;
    r.px = 100;
    r.qty = 20;
    r.now_us = 1000000;
    r.side = static_cast<uint8_t>(pitlane::Side::Buy);
    r.is_open = 1;
    r.rest = 1;
    expect(g.check(r) == pitlane::PreCode::Notional, "pre notional");
    r.px = 10;
    r.qty = 10;
    expect(g.check(r) == pitlane::PreCode::Ok, "pre ok");
    expect(g.on_submit(r, 7), "pre submit");
    expect(g.active_count() == 1, "pre active");
    r.now_us += 1000;
    expect(g.on_submit(r, 8), "pre submit two");
    r.now_us += 1000;
    expect(g.check(r) == pitlane::PreCode::AcctFlow1s, "pre account flow");
    r.now_us += 1000000;
    expect(g.check(r) == pitlane::PreCode::Ok, "pre flow clears");
    pitlane::PreReq s = r;
    s.side = static_cast<uint8_t>(pitlane::Side::Sell);
    s.is_open = 0;
    s.px = 9;
    uint64_t clash = 0;
    expect(g.check(s, &clash) == pitlane::PreCode::SelfTrade, "pre self trade");
    expect(clash != 0, "pre clash id");
    s.px = 11;
    expect(g.check(s) == pitlane::PreCode::Ok, "pre no cross");
    g.open_blacklist().add(5);
    expect(g.check(r) == pitlane::PreCode::OpenBlacklist, "pre open blacklist");
    g.open_blacklist().clear();
    g.close_blacklist().add(5);
    expect(g.check(s) == pitlane::PreCode::CloseBlacklist, "pre close blacklist");
    g.close_blacklist().clear();
    g.halt(true);
    expect(g.check(r) == pitlane::PreCode::Halted, "pre halted");
    g.halt(false);
    g.on_done(7);
    g.on_done(8);
    expect(g.active_count() == 0, "pre active done");

    pitlane::PreTradeGuard c{};
    pitlane::PreLimits cl{};
    cl.max_cancel_ratio_ppm = 100000;
    c.set_limits(cl);
    c.reset();
    pitlane::PreReq q{};
    q.acct = 2;
    q.name = 9;
    q.px = 1;
    q.qty = 1;
    q.side = static_cast<uint8_t>(pitlane::Side::Buy);
    q.rest = 0;
    q.now_us = 0;
    expect(c.on_submit(q, 1) && c.on_submit(q, 2), "pre ratio submits");
    c.on_cancel(2, 0, 1, 1, 10);
    expect(c.check(q) == pitlane::PreCode::CancelRatio, "pre cancel ratio");

    pitlane::PreTradeGuard a{};
    pitlane::PreLimits al{};
    al.max_cancel_amount_ratio_ppm = 100000;
    al.cancel_amount_window_s = 10;
    a.set_limits(al);
    a.reset();
    pitlane::PreReq w{};
    w.acct = 4;
    w.name = 3;
    w.px = 100;
    w.qty = 10;
    w.side = static_cast<uint8_t>(pitlane::Side::Buy);
    w.rest = 0;
    w.now_us = 500000;
    expect(a.on_submit(w, 11), "pre amount submit");
    a.on_cancel(4, 0, 100, 5, 600000);
    expect(a.check(w) == pitlane::PreCode::AmountCancelRatio, "pre amount ratio");
}

static void check_link() {
    pitlane::OrderLink l;
    expect(l.phase() == pitlane::kLinkDown, "link starts down");
    expect(!l.note_order(1, 1000), "link rejects while down");
    l.up(1000);
    expect(l.note_order(1, 1000), "link order");
    expect(!l.note_order(1, 1000), "link duplicate");
    expect(l.inflight() == 1, "link inflight");
    expect(l.note_ack(1, 1200) && l.inflight() == 0, "link ack");

    expect(l.note_order(2, 2000), "link order 2");
    expect(l.tick(302001) == 1 && l.phase() == pitlane::kLinkRecovering, "link ack timeout");
    expect(l.inflight() == 0 && l.stats().late_acks == 1, "link timeout cleared");
    expect(l.recovered(303000) && l.phase() == pitlane::kLinkUp, "link recovered");

    expect(l.note_seq(10) && l.note_seq(11), "link seq ok");
    expect(!l.note_seq(13) && l.stats().gaps == 1 && l.phase() == pitlane::kLinkRecovering, "link seq gap");
    expect(l.recovered(304000), "link gap recovered");

    expect(l.note_order(3, 305000), "link order 3");
    expect(l.note_cancel(3, 305000) && l.note_cancel_ack(3, 306000), "link cancel ack");
    expect(l.inflight() == 0, "link cancel inflight");

    pitlane::OrderLink d;
    d.up(0);
    expect(d.note_order(4, 0) && d.note_ack(4, 1, true), "link reject 1");
    expect(d.note_order(5, 2) && d.note_ack(5, 3, true), "link reject 2");
    expect(d.phase() == pitlane::kLinkDegraded, "link degraded by rejects");
    d.down(4);
    expect(d.resync_gen() == 1 && d.phase() == pitlane::kLinkDown, "link down resync");
}

static void check_exec_algo() {
    using namespace pitlane;
    ExecCfg cfg{};
    cfg.name = 7;
    cfg.side = static_cast<uint8_t>(Side::Buy);
    cfg.style = static_cast<uint8_t>(ExecStyle::Twap);
    cfg.total = 1000;
    cfg.t0_us = 0;
    cfg.end_us = 100000000;
    cfg.lot = 100;
    cfg.tick = 1;
    cfg.max_clip = 200;
    cfg.catchup_ppm = 200000;
    cfg.reorder_us = 1000000;
    cfg.stale_us = 200000000;

    ExecAlgo a;
    ExecCfg bad = cfg;
    bad.end_us = 0;
    expect(!a.open(bad, 0), "algo rejects a zero horizon");
    expect(a.state() == ExecState::Failed, "algo fails closed");

    expect(a.open(cfg, 0), "algo opens");
    expect(a.target_at(0) == 0, "algo target at start");
    expect(a.target_at(50000000) == 500, "linear target at half");
    expect(a.target_at(100000000) == 1000, "linear target at end");

    QuoteTop q{};
    q.bid[0] = 1000;
    q.ask[0] = 1001;
    q.bid_qty = 5000;
    q.ask_qty = 5000;
    a.on_quote(q, 0);
    ExecCmd c{};
    expect(a.poll(0, &c, 1) == 0, "algo waits at a zero target");

    // Ten percent through the schedule: 100 shares are due, below the 200 to
    // catch up, so the first child joins the bid.
    expect(a.poll(10000000, &c, 1) == 1, "passive child");
    expect(c.kind == kExecPlace && c.side == static_cast<uint8_t>(Side::Buy), "place buy");
    expect(c.px == 1000 && c.qty == 100, "passive price and size");
    expect(c.style == static_cast<uint8_t>(Style::Day), "passive rests");
    expect(c.id == 1, "first child id");

    // The market moves away; after the reorder lag the child is pulled.
    QuoteTop moved{};
    moved.bid[0] = 1002;
    moved.ask[0] = 1003;
    moved.bid_qty = 5000;
    moved.ask_qty = 5000;
    a.on_quote(moved, 11000000);
    expect(a.poll(11000000, &c, 1) == 1 && c.kind == kExecCancel && c.id == 1, "cancel stale child");
    a.on_cancel();

    // Halfway: 400 more are due after the 100 cancelled, which is above the
    // catch-up band, so the next child crosses.
    expect(a.poll(50000000, &c, 1) == 1, "aggressive child");
    expect(c.px == 1003 && c.qty == 200, "crossing price and clip");
    expect(c.style == static_cast<uint8_t>(Style::Immediate), "aggressive is immediate");
    a.on_fill(200, 1003);
    expect(a.done() == 200 && a.remaining() == 800, "fill updates progress");

    // Participation caps the next child at half of the displayed size.
    ExecCfg part = cfg;
    part.participation_ppm = 500000;
    part.lot = 10;
    ExecAlgo b;
    expect(b.open(part, 0), "participation opens");
    QuoteTop thin{};
    thin.bid[0] = 1000;
    thin.ask[0] = 1001;
    thin.bid_qty = 100;
    thin.ask_qty = 100;
    b.on_quote(thin, 90000000);
    expect(b.poll(90000000, &c, 1) == 1, "participation child");
    expect(c.qty == 50, "participation caps at half the ask");

    // A volume curve puts 25% of the parent through the first of two buckets.
    const int64_t weights[2] = {1, 3};
    ExecCfg vwap = cfg;
    vwap.style = static_cast<uint8_t>(ExecStyle::Vwap);
    vwap.curve = weights;
    vwap.curve_n = 2;
    ExecAlgo v;
    expect(v.open(vwap, 0), "vwap opens");
    expect(v.target_at(50000000) == 250, "curve target after bucket one");
    expect(v.target_at(75000000) == 625, "curve target mid bucket two");

    // A locked market sends one child and then waits.
    ExecCfg touch = cfg;
    touch.one_touch = 1;
    ExecAlgo t;
    expect(t.open(touch, 0), "one touch opens");
    QuoteTop locked{};
    locked.bid[0] = 1100;
    locked.ask[0] = 1100;
    t.on_quote(locked, 60000000);
    expect(t.poll(60000000, &c, 1) == 1 && c.px == 1100, "one touch crosses once");
    t.on_fill(c.qty, c.px);
    expect(t.poll(61000000, &c, 1) == 0, "one touch then waits");
    expect(t.row().one_touch_used == 1, "one touch flag");

    expect(a.pause() && a.state() == ExecState::Paused, "algo pause");
    expect(a.resume() && a.state() != ExecState::Paused, "algo resume");
    expect(a.stop() && a.state() == ExecState::Stopped, "algo stop");
    expect(!a.active(), "stopped algo is inactive");
}

static void check_basket_ctl() {
    using namespace pitlane;
    BasketLeg leg{};
    leg.name = 7;
    leg.target = 1000;
    BasketLegCtl ctl{};
    ctl.clip = 300;
    ctl.granularity = 100;
    ctl.tolerance = 50;
    QuoteTop q{};
    q.bid[0] = 1000;
    q.ask[0] = 1001;
    BasketChild ch{};
    expect(basket_child(leg, ctl, 0, q, 1, 10, ch), "basket child");
    expect(ch.ok && ch.side == static_cast<uint8_t>(Side::Buy), "basket buys the gap");
    expect(ch.qty == 300 && ch.px == 1001, "basket clip crosses");
    expect(ch.style == static_cast<uint8_t>(Style::Immediate), "opposite price is immediate");

    ctl.price_type = 1;
    expect(basket_child(leg, ctl, 0, q, 1, 10, ch) && ch.px == 1000, "own price joins");
    expect(ch.style == static_cast<uint8_t>(Style::Day), "own price rests");

    expect(!basket_child(leg, ctl, 980, q, 1, 10, ch), "inside tolerance is left alone");
    expect(basket_child(leg, ctl, -100, q, 1, 10, ch) && ch.gap == 1100, "short net widens the gap");

    ctl.begin_us = 20;
    expect(!basket_child(leg, ctl, 0, q, 1, 10, ch), "before the window");
    ctl.begin_us = 0;
    ctl.end_us = 5;
    expect(!basket_child(leg, ctl, 0, q, 1, 10, ch), "after the window");
    ctl.late_us = 10;
    expect(basket_child(leg, ctl, 0, q, 1, 10, ch), "late tolerance trades");
    expect(ch.style == static_cast<uint8_t>(Style::Immediate) && ch.px == 1001, "late tolerance crosses");
    ctl.late_us = 0;

    ctl.end_us = 0;
    ctl.one_touch = 1;
    QuoteTop locked{};
    locked.bid[0] = 1200;
    locked.ask[0] = 1200;
    uint8_t touched = 0;
    expect(basket_child(leg, ctl, 0, locked, 1, 10, ch, &touched), "locked one touch");
    expect(ch.px == 1200 && ch.reason == 1 && touched == 1, "locked sends once");
    expect(!basket_child(leg, ctl, 0, locked, 1, 10, ch, &touched), "locked then waits");

    BasketBook<4> book;
    expect(book.open(9) && book.put(7, 1000), "basket book opens");
    BasketLegCtl saved{};
    saved.clip = 250;
    saved.priority = 2;
    expect(book.set_ctl(7, saved), "basket ctl set");
    expect(book.ctl_at(0) && book.ctl_at(0)->clip == 250 && book.ctl_at(0)->priority == 2, "basket ctl stored");
    saved.granularity = 0;
    expect(!book.set_ctl(7, saved), "zero granularity rejected");
}

static void check_exec_limit() {
    using namespace pitlane;
    ExecCfg cfg{};
    cfg.name = 7;
    cfg.side = static_cast<uint8_t>(Side::Sell);
    cfg.style = static_cast<uint8_t>(ExecStyle::Pass);
    cfg.total = 1000;
    cfg.t0_us = 0;
    cfg.end_us = 100000000;
    cfg.lot = 100;
    cfg.tick = 1;
    cfg.up_limit = 1100;
    cfg.down_limit = 900;

    ExecAlgo a;
    ExecCmd c{};
    expect(a.open(cfg, 0), "limit algo opens");
    QuoteTop up{};
    up.bid[0] = 1100;
    up.ask[0] = 1101;
    a.on_quote(up, 0);
    expect(a.poll(0, &c, 1) == 0 && a.state() == ExecState::Stopped, "sell stops at the up limit");

    cfg.allow_at_limit = 1;
    expect(a.open(cfg, 0), "algo reopens with the allowance");
    a.on_quote(up, 0);
    expect(a.poll(1000, &c, 1) == 1, "allow_at_limit keeps selling");

    cfg.side = static_cast<uint8_t>(Side::Buy);
    cfg.allow_at_limit = 0;
    expect(a.open(cfg, 0), "buy opens");
    QuoteTop down{};
    down.bid[0] = 899;
    down.ask[0] = 900;
    down.bid_qty = 1000;
    down.ask_qty = 1000;
    a.on_quote(down, 0);
    expect(a.poll(0, &c, 1) == 0 && a.state() == ExecState::Stopped, "buy stops at the down limit");

    ExecCfg bad = cfg;
    bad.up_limit = -1;
    expect(!a.open(bad, 0), "negative limit rejected");
    bad = cfg;
    bad.up_limit = 900;
    bad.down_limit = 900;
    expect(!a.open(bad, 0), "crossed limits rejected");
}

static void check_auction() {
    using namespace pitlane;
    AucCfg cfg{};
    cfg.name = 7;
    cfg.side = static_cast<uint8_t>(Side::Buy);
    cfg.kind = kAucOpen;
    cfg.px_mode = kAucLimit;
    cfg.total = 1000;
    cfg.begin_us = 1000;
    cfg.end_us = 2000;
    cfg.lot = 100;
    cfg.tick = 1;
    cfg.limit_px = 1000;
    cfg.id_base = 500;
    cfg.up_limit = 1100;
    cfg.down_limit = 900;
    cfg.undo = 1;

    AuctionAlgo a;
    AucCfg bad = cfg;
    bad.total = 0;
    expect(!a.open(bad, 0) && a.state() == AucState::Failed, "auction rejects zero quantity");
    bad = cfg;
    bad.end_us = cfg.begin_us;
    expect(!a.open(bad, 0), "auction rejects the horizon");
    bad = cfg;
    bad.limit_px = 0;
    expect(!a.open(bad, 0), "auction requires a limit price");
    PxBand broken[] = {{0, 100, 0}};
    bad = cfg;
    bad.bands = broken;
    bad.band_n = 1;
    expect(!a.open(bad, 0), "auction rejects a broken band");

    expect(a.open(cfg, 0), "auction opens waiting");
    expect(a.state() == AucState::Wait, "auction waits");
    QuoteTop q{};
    q.bid[0] = 998;
    q.ask[0] = 999;
    q.bid_qty = 5000;
    q.ask_qty = 5000;
    a.on_quote(q, 0);
    ExecCmd c{};
    expect(a.poll(500, &c, 1) == 0, "auction waits before the window");
    expect(a.poll(1000, &c, 1) == 1, "auction places at the bell");
    expect(c.kind == kExecPlace && c.id == 500 && c.px == 1000 && c.qty == 1000, "auction child");
    expect(c.style == static_cast<uint8_t>(Style::Immediate), "auction crosses");
    expect(a.poll(1500, &c, 1) == 0, "one working child");
    a.on_fill(400, 1000);
    expect(a.done() == 400 && a.remaining() == 600, "partial auction fill");
    expect(a.poll(2001, &c, 1) == 0 && a.state() == AucState::Roll, "auction rolls the remainder");
    expect(a.roll_out() == 600 && a.state() == AucState::Done, "roll hands out the remainder");
    expect(a.roll_out() == 0, "roll is taken once");

    cfg.undo = 0;
    expect(a.open(cfg, 0), "auction opens without undo");
    a.on_quote(q, 0);
    expect(a.poll(1000, &c, 1) == 1, "second auction child");
    a.on_fill(400, 1000);
    expect(a.poll(2001, &c, 1) == 0 && a.state() == AucState::Stopped, "auction stops unfinished");
    expect(a.remaining() == 600 && a.roll_out() == 0, "no roll without undo");

    cfg.undo = 1;
    expect(a.open(cfg, 0), "auction reopens");
    a.on_quote(q, 0);
    expect(a.poll(1000, &c, 1) == 1 && c.id == 500, "first try");
    a.on_reject();
    expect(a.poll(1000, &c, 1) == 1 && c.id == 501, "replaces a reject");
    a.on_cancel();
    expect(a.poll(1000, &c, 1) == 1 && c.id == 502, "replaces a cancel");

    AucCfg sell{};
    sell.name = 7;
    sell.side = static_cast<uint8_t>(Side::Sell);
    sell.px_mode = kAucTouch;
    sell.total = 300;
    sell.begin_us = 0;
    sell.end_us = 100;
    sell.lot = 100;
    sell.tick = 1;
    sell.max_clip = 200;
    sell.adjust_ticks = 1;
    sell.id_base = 900;
    expect(a.open(sell, 0), "touch auction opens");
    QuoteTop q2{};
    q2.bid[0] = 1000;
    q2.ask[0] = 1001;
    q2.bid_qty = 1000;
    q2.ask_qty = 1000;
    a.on_quote(q2, 0);
    expect(a.poll(10, &c, 1) == 1 && c.qty == 200 && c.px == 999 && c.id == 900, "touch clips a sell");
    a.on_fill(200, 999);
    expect(a.poll(20, &c, 1) == 1 && c.qty == 100 && c.id == 901, "second clip");

    AucCfg mid = sell;
    mid.side = static_cast<uint8_t>(Side::Buy);
    mid.px_mode = kAucMid;
    mid.adjust_ticks = 0;
    mid.total = 100;
    mid.max_clip = 0;
    mid.id_base = 950;
    expect(a.open(mid, 0), "mid auction opens");
    a.on_quote(q2, 0);
    expect(a.poll(10, &c, 1) == 1 && c.px == 1000 && c.qty == 100, "mid price");

    AucCfg band = cfg;
    PxBand bands[] = {{0, 1000, 1}, {1000, 5000, 5}, {5000, INT64_MAX, 10}};
    band.bands = bands;
    band.band_n = 3;
    band.limit_px = 1002;
    expect(a.open(band, 0), "band auction opens");
    a.on_quote(q, 0);
    expect(a.poll(1000, &c, 1) == 1 && c.px == 1005, "band tick rounds a buy up");

    AucCfg guard = cfg;
    guard.down_limit = 900;
    guard.allow_at_limit = 0;
    expect(a.open(guard, 0), "guard auction opens");
    QuoteTop q3{};
    q3.bid[0] = 899;
    q3.ask[0] = 900;
    a.on_quote(q3, 0);
    expect(a.poll(1000, &c, 1) == 0 && a.state() == AucState::Stopped, "down limit blocks a buy");
    guard.allow_at_limit = 1;
    expect(a.open(guard, 0), "guard auction reopens");
    a.on_quote(q3, 0);
    expect(a.poll(1000, &c, 1) == 1, "allow_at_limit keeps bidding");

    AucCfg sell_guard = sell;
    sell_guard.up_limit = 1010;
    sell_guard.allow_at_limit = 0;
    expect(a.open(sell_guard, 0), "sell guard opens");
    QuoteTop q4{};
    q4.bid[0] = 1010;
    q4.ask[0] = 1011;
    a.on_quote(q4, 0);
    expect(a.poll(10, &c, 1) == 0 && a.state() == AucState::Stopped, "up limit blocks a sell");

    expect(a.open(sell, 0), "pause test opens");
    a.on_quote(q2, 0);
    expect(a.pause(), "pause");
    expect(a.poll(10, &c, 1) == 0, "paused auction is quiet");
    expect(a.resume() && a.poll(10, &c, 1) == 1, "resume places");
    expect(a.stop() && a.state() == AucState::Stopped, "stop");
}

static void check_rms_tick_lot() {
    using namespace pitlane;
    RmsLimits lim{};
    lim.tick_size = 5;
    lim.lot_size = 100;
    Rms rms;
    rms.set(lim);
    RmsReq req{};
    req.acct = 1;
    req.name = 7;
    req.side = static_cast<uint8_t>(Side::Buy);
    req.px = 1002;
    req.qty = 200;
    req.free_cash = 100000000;
    expect(rms.check(req) == RmsCode::Tick, "off-tick price rejected");
    req.px = 1005;
    req.qty = 150;
    expect(rms.check(req) == RmsCode::Lot, "odd lot rejected");
    req.qty = 200;
    expect(rms.check(req) == RmsCode::Ok, "aligned price and lot pass");
    expect(std::strcmp(rms_code_name(RmsCode::Tick), "price tick") == 0, "tick reason name");
}

static void check_unlock() {
    pitlane::UnlockCfg cfg{};
    cfg.lot = 100;
    pitlane::UnlockPlan plan;
    expect(plan.open(cfg, 1000, 600), "unlock open");
    expect(plan.goal() == 600, "goal is the smaller side");
    pitlane::UnlockReq req[2]{};
    expect(plan.step(req, 2) == 2, "first paired step");
    expect(req[0].leg == 0 && req[0].side == 2 && req[0].qty == 600, "sell the long leg");
    expect(req[1].leg == 1 && req[1].side == 1 && req[1].qty == 600, "buy back the short leg");
    expect(plan.step(req, 2) == 0, "pending blocks a second step");

    expect(plan.on_fill(0, 600), "long fill");
    expect(plan.step(req, 2) == 0, "short child still in flight");
    expect(plan.on_cancel(1, 300), "partial short cancel");
    expect(plan.step(req, 2) == 1 && req[0].leg == 1 && req[0].qty == 300, "cancelled part is re-emitted");
    expect(!plan.done(), "not done while the short leg is open");
    expect(plan.on_fill(1, 300), "first short fill");
    expect(plan.step(req, 2) == 0, "live child covers the gap");
    expect(plan.on_fill(1, 300), "second short fill");
    expect(plan.done(), "both legs at the goal");
    const pitlane::UnlockRow row = plan.row();
    expect(row.done_long == 600 && row.done_short == 600 && row.done == 1, "done counters");
    expect(row.residual_long == 400 && row.residual_short == 0, "residual long");

    pitlane::UnlockCfg clip{};
    clip.lot = 100;
    clip.clip = 250;
    pitlane::UnlockPlan cut;
    expect(cut.open(clip, 500, 500), "clip open");
    expect(cut.step(req, 2) == 2 && req[0].qty == 200 && req[1].qty == 200, "clip rounds down to lots");
    expect(cut.on_cancel(0, 200) && cut.on_reject(1, 200), "cancel and reject release the clips");
    expect(cut.step(req, 2) == 2 && req[0].qty == 200 && req[1].qty == 200, "released clips are re-emitted");
    expect(!cut.on_cancel(0, 0), "zero cancel refused");
    expect(!cut.on_reject(2, 100), "bad leg refused");

    pitlane::UnlockCfg one{};
    one.lot = 100;
    pitlane::UnlockPlan single;
    expect(single.open(one, 300, 300), "one leg open");
    expect(single.step(req, 1) == 1 && req[0].leg == 0 && req[0].qty == 300, "cap one takes the long leg");
    expect(single.step(req, 1) == 1 && req[0].leg == 1 && req[0].qty == 300, "then the short leg");

    pitlane::UnlockCfg floor{};
    floor.lot = 100;
    pitlane::UnlockPlan odd;
    expect(odd.open(floor, 1050, 987), "odd open");
    expect(odd.goal() == 900, "odd goal rounds down");
    const pitlane::UnlockRow left = odd.row();
    expect(left.residual_long == 1050 && left.residual_short == 987, "residual before work");
    expect(odd.step(req, 2) == 2 && req[0].qty == 900 && req[1].qty == 900, "paired 900");
    expect(odd.on_fill(0, 900) && odd.on_fill(1, 900), "both fill");
    const pitlane::UnlockRow rem = odd.row();
    expect(rem.residual_long == 150 && rem.residual_short == 87 && rem.done == 1, "lot remainders");

    pitlane::UnlockPlan empty;
    expect(empty.open(floor, 0, 0), "empty open");
    expect(empty.done() && empty.step(req, 2) == 0, "nothing to unlock");

    pitlane::UnlockPlan bad;
    expect(!bad.open(pitlane::UnlockCfg{0, 0, 0}, 100, 100), "zero lot refused");
    expect(!bad.open(floor, -1, 100), "negative quantity refused");
    pitlane::UnlockCfg inverted{};
    inverted.lot = 100;
    inverted.clip = 100;
    inverted.min_clip = 200;
    expect(!bad.open(inverted, 100, 100), "min clip above clip refused");

    pitlane::UnlockPlan over;
    expect(over.open(floor, 100, 100), "overfill open");
    expect(over.step(req, 2) == 2, "overfill step");
    expect(over.on_fill(0, 150), "overfill clamped");
    expect(!over.on_fill(0, 50), "no room after the goal");
}

static void check_netting() {
    pitlane::NetCfg cfg{};
    pitlane::NetBook<8> book;
    expect(book.open(cfg), "net open");
    expect(book.add({1, 7, 11, 1, 300}), "net buy 300");
    expect(book.add({1, 7, 11, 1, 200}), "net buy 200");
    expect(book.add({1, 7, 11, 2, 100}), "net sell 100");
    pitlane::NetLeg legs[8]{};
    pitlane::NetReport rep{};
    expect(book.legs(legs, 8, rep) == 1, "one net leg");
    expect(legs[0].side == 1 && legs[0].qty == 400 && legs[0].name == 11, "net buy 400");
    expect(rep.gross == 600 && rep.net == 400 && rep.saved == 200 && rep.residual == 0, "net counters");
    expect(rep.rows_in == 3 && rep.rows_out == 1 && rep.dropped == 0, "net rows");

    pitlane::NetBook<8> flip;
    expect(flip.open(cfg), "flip open");
    expect(flip.add({1, 7, 11, 1, 100}) && flip.add({1, 7, 11, 2, 250}), "flip rows");
    expect(flip.legs(legs, 8, rep) == 1 && legs[0].side == 2 && legs[0].qty == 150, "net sell 150");

    pitlane::NetBook<8> flat;
    expect(flat.open(cfg), "flat open");
    expect(flat.add({1, 7, 11, 1, 100}) && flat.add({1, 7, 11, 2, 100}), "offset rows");
    expect(flat.legs(legs, 8, rep) == 0 && rep.dropped == 1 && rep.saved == 200, "fully offset");

    pitlane::NetBook<8> sep;
    expect(sep.open(cfg), "sep open");
    expect(sep.add({1, 7, 11, 1, 100}) && sep.add({1, 8, 11, 2, 100}), "two counters");
    expect(sep.legs(legs, 8, rep) == 2, "counters stay apart");
    expect(legs[0].counter == 7 && legs[1].counter == 8, "counter order");

    pitlane::NetCfg acct{};
    acct.scope = pitlane::kNetAcct;
    pitlane::NetBook<8> across;
    expect(across.open(acct), "acct open");
    expect(across.add({1, 7, 11, 1, 100}) && across.add({1, 8, 11, 2, 100}), "acct rows");
    expect(across.legs(legs, 8, rep) == 0 && rep.dropped == 1, "one account nets across counters");

    pitlane::NetCfg name{};
    name.scope = pitlane::kNetName;
    pitlane::NetBook<8> wide;
    expect(wide.open(name), "name open");
    expect(wide.add({1, 7, 11, 1, 300}) && wide.add({2, 8, 11, 2, 100}), "name rows");
    expect(wide.legs(legs, 8, rep) == 1 && legs[0].qty == 200, "names net across accounts");

    pitlane::NetCfg lot{};
    lot.lot = 100;
    pitlane::NetBook<8> round;
    expect(round.open(lot), "lot open");
    expect(round.add({1, 7, 11, 1, 150}), "odd net");
    expect(round.legs(legs, 8, rep) == 1 && legs[0].qty == 100, "lot floor");
    expect(rep.residual == 50 && rep.net == 100 && rep.saved == 0, "lot residual");

    pitlane::NetBook<8> bad;
    expect(bad.open(cfg), "bad open");
    expect(!bad.add({1, 7, 11, 3, 100}), "bad side");
    expect(!bad.add({1, 7, 11, 1, 0}), "zero quantity");
    expect(!bad.add({1, 7, 0, 1, 100}), "no name");
    expect(!bad.add({0, 7, 11, 1, 100}), "no account");
    expect(bad.legs(legs, 8, rep) == 0 && rep.invalid == 4 && rep.rows_in == 0, "invalid rows");

    pitlane::NetBook<2> small;
    expect(small.open(cfg), "small open");
    expect(small.add({1, 7, 11, 1, 100}) && small.add({1, 7, 12, 1, 100}), "two keys");
    expect(!small.add({1, 7, 13, 1, 100}), "third key overflows");
    expect(small.add({1, 7, 11, 2, 100}), "existing key still accepted");
    expect(small.legs(nullptr, 2, rep) == -1, "missing output refused");
    expect(small.legs(legs, 2, rep) == 1 && legs[0].name == 12 && rep.overflow == 1,
           "zero key dropped, order kept");
    expect(sep.legs(legs, 1, rep) == -1, "short output refused");

    pitlane::NetBook<4> closed;
    expect(!closed.open(pitlane::NetCfg{0, 1}), "unknown scope refused");
    expect(!closed.open(pitlane::NetCfg{pitlane::kNetAcct, 0}), "zero lot refused");
    expect(!closed.add({1, 7, 11, 1, 100}), "closed book refuses rows");
}

static void check_pair_package() {
    pitlane::PairPackageBook<4> book;
    pitlane::PairPackageCfg cfg{};
    cfg.name_a = 1;
    cfg.name_b = 3;
    cfg.qty_a = 10;
    cfg.ratio_ppm = 1500000;  // 1.5 B per A
    cfg.fx_ppm = 1000000;
    pitlane::pair_package_note(cfg.note, sizeof cfg.note, "spread one");
    const uint32_t id = book.create(cfg);
    expect(id == 1, "package create id");

    pitlane::PairPackage p{};
    expect(book.get(id, p), "package get");
    expect(p.state == pitlane::kPkgOpen && p.qty_b == 15, "package target");
    expect(std::strcmp(p.note, "spread_one") == 0, "note sanitised");

    pitlane::PairPackageCfg bad = cfg;
    bad.name_b = bad.name_a;
    expect(book.create(bad) == 0, "same leg refused");
    bad = cfg;
    bad.qty_a = 0;
    expect(book.create(bad) == 0, "zero qty refused");
    bad = cfg;
    bad.ratio_ppm = 0;
    expect(book.create(bad) == 0, "zero ratio refused");

    expect(book.fill(id, 1, 4) && book.fill(id, 2, 6), "package partial fill");
    expect(book.get(id, p) && p.state == pitlane::kPkgOpen, "still open");
    expect(!book.fill(id, 2, 10), "overfill refused");
    expect(!book.fill(id, 3, 1), "bad leg refused");

    pitlane::PairPackageCfg next = cfg;
    next.qty_a = 3;
    expect(!book.update(id, next), "update below filled refused");
    next.qty_a = 20;
    expect(book.update(id, next) && book.get(id, p) && p.qty_a == 20 && p.qty_b == 30, "package update");

    expect(book.close(id) == false, "close before complete refused");
    expect(book.fill(id, 1, 16) && book.fill(id, 2, 24), "package complete");
    expect(book.get(id, p) && p.state == pitlane::kPkgClosed, "auto close");

    const uint32_t id2 = book.create(cfg);
    expect(id2 == 2, "second id");
    expect(book.cancel(id2) && book.get(id2, p) && p.state == pitlane::kPkgCancelled, "cancel");
    expect(!book.cancel(id2), "double cancel refused");

    pitlane::PairPackage rows[4]{};
    expect(book.list(rows, 4) == 2 && rows[0].id == 1 && rows[1].id == 2, "list order");
    expect(book.list(rows, 1) == -1, "short list refused");

    pitlane::PairPackageBook<1> one;
    expect(one.create(cfg) == 1, "one slot");
    expect(one.create(cfg) == 0, "full book refused");

    pitlane::PairPackage saved = p;
    saved.id = 9;
    saved.state = pitlane::kPkgOpen;
    saved.filled_a = 0;
    saved.filled_b = 0;
    expect(book.restore(saved), "restore");
    expect(book.create(cfg) == 10, "restore advances id");
    saved.id = 9;
    expect(!book.restore(saved), "duplicate restore refused");
}

static void check_credit_book() {
    pitlane::CreditBook<2> book;
    pitlane::CreditTerms t{};
    t.asset_balance = 2000000;
    t.total_debt = 1000000;
    t.fund_debt = 600000;
    t.stock_debt = 400000;
    t.finance_quota = 500000;
    t.shortsell_quota = 300000;
    t.source = pitlane::kCreditSourceManual;
    expect(book.set(1, t), "credit set");
    expect(book.count() == 1, "credit count");
    pitlane::CreditTerms out{};
    expect(book.get(1, out) && out.finance_quota == 500000, "credit get");
    expect(pitlane::credit_assure_ppm(out) == 2000000, "assure derived");

    out.assure_ratio_ppm = 1800000;
    expect(pitlane::credit_assure_ppm(out) == 1800000, "assure reported verbatim");
    expect(pitlane::credit_finance_left(out, 100000) == 400000, "finance left");
    expect(pitlane::credit_shortsell_left(out, 400000) == 0, "shortsell floor");

    pitlane::CreditCash cash{};
    cash.short_qty = 7;
    cash.special_short = 2;
    expect(book.set_cash(1, cash), "credit cash");
    pitlane::CreditCash back{};
    expect(book.get_cash(1, back) && back.short_qty == 7 && back.special_short == 2, "credit cash roundtrip");

    pitlane::CreditTerms neg = t;
    neg.finance_quota = -1;
    expect(!book.set(2, neg), "negative quota refused");
    expect(book.set(2, t) && book.count() == 2, "second account");
    expect(!book.set(3, t), "full book refused");

    uint32_t accts[2]{};
    pitlane::CreditTerms terms[2]{};
    pitlane::CreditCash cash_rows[2]{};
    expect(book.list(accts, terms, cash_rows, 2) == 2 && accts[0] == 1 && accts[1] == 2, "credit list");
    expect(book.erase(1) && book.count() == 1, "credit erase");
}

struct FakeExec {
    int sent{0};
    int64_t qty[16]{};
    uint8_t side[16]{};
    uint8_t off[16]{};
    uint8_t biz[16]{};
    int64_t net{0};
};

static int fake_send(void* user, const pitlane::Intent& in) {
    auto* f = static_cast<FakeExec*>(user);
    if (f->sent < 16) {
        f->qty[f->sent] = in.qty;
        f->side[f->sent] = in.side;
        f->off[f->sent] = static_cast<uint8_t>(in.off);
        f->biz[f->sent] = in.biz;
        ++f->sent;
    }
    f->net += in.side == static_cast<uint8_t>(pitlane::Side::Buy) ? in.qty : -in.qty;
    return 0;
}

static int64_t fake_net(void* user, uint32_t) { return static_cast<FakeExec*>(user)->net; }

static void fake_top(void*, uint32_t, pitlane::ManualQuote& q) {
    q.bid = 100;
    q.ask = 101;
}

static void check_manual_algo() {
    pitlane::IdMint mint(1, 1);
    FakeExec f{};
    pitlane::ManualBook book;
    book.open(&mint, fake_send, &f, fake_top, nullptr, fake_net, &f);

    // Plain TWAP: four slices of one, then the parent finishes.
    const int64_t h = 4000000;
    const int64_t s = 1000000;
    expect(book.algo_open(1, 7, static_cast<uint8_t>(pitlane::Side::Buy), 4, h, s, 1000000) == pitlane::kOpsOk, "algo open");
    for (int64_t t = 1000000; t <= 5000000; t += s) (void)book.on_beat(t, 0, 0);
    expect(f.sent == 4, "twap sent four slices");
    expect(f.net == 4, "twap net");
    expect(book.algos_active() == 0, "twap finished");
    expect(book.algo_pause(1) == pitlane::kOpsBadReq, "finished algo refused");

    // Pause stops slices and resume restarts the clock.
    f = FakeExec{};
    expect(book.algo_open(2, 7, static_cast<uint8_t>(pitlane::Side::Buy), 4, h, s, 1000000) == pitlane::kOpsOk,
           "algo2 open");
    expect(book.algo_pause(2) == pitlane::kOpsOk, "algo2 pause");
    (void)book.on_beat(1000000, 0, 0);
    expect(f.sent == 0, "paused algo quiet");
    expect(book.algo_resume(2, 1000000) == pitlane::kOpsOk, "algo2 resume");
    (void)book.on_beat(1000000, 0, 0);
    expect(f.sent == 1, "resumed algo sends");
    expect(book.algo_cancel(2) == pitlane::kOpsOk, "algo2 cancel");
    (void)book.on_beat(2000000, 0, 0);
    expect(f.sent == 1 && book.algos_active() == 0, "cancelled algo quiet");
    expect(book.algo_cancel(2) == pitlane::kOpsBadReq, "double cancel refused");

    // Update reschedules the remaining parent; zero keeps a value.
    f = FakeExec{};
    expect(book.algo_open(3, 7, static_cast<uint8_t>(pitlane::Side::Buy), 4, h, s, 1000000) == pitlane::kOpsOk,
           "algo3 open");
    expect(book.algo_update(3, 8, 8000000, 2000000, 1000000) == pitlane::kOpsOk, "algo3 update");
    (void)book.on_beat(1000000, 0, 0);
    expect(f.sent == 1 && f.qty[0] == 2, "updated slice size");
    expect(book.algo_update(3, 0, 0, 0, 1000000) == pitlane::kOpsOk, "algo3 no-op update");
    expect(book.algo_update(99, 1, 0, 0, 1000000) == pitlane::kOpsBadReq, "unknown update refused");
    expect(book.algo_cancel(3) == pitlane::kOpsOk && book.algos_active() == 0, "algo3 cancel");

    // T0 buy: from net 5 open to 8, then unwind back to 5.
    f = FakeExec{};
    f.net = 5;
    expect(book.algo_open(4, 9, static_cast<uint8_t>(pitlane::Side::Buy), 3, 20000000, s, 1000000, 0, 1) ==
               pitlane::kOpsOk,
           "t0 open");
    for (int64_t t = 1000000; t <= 10000000; t += s) (void)book.on_beat(t, 0, 0);
    expect(f.sent == 6, "t0 round trip children");
    expect(f.net == 5, "t0 flat to entry net");
    expect(book.algos_active() == 0, "t0 finished");

    // T0 sell from flat unwinds back to zero.
    f = FakeExec{};
    expect(book.algo_open(5, 9, static_cast<uint8_t>(pitlane::Side::Sell), 2, 20000000, s, 1000000, 0, 1) ==
               pitlane::kOpsOk,
           "t0 sell open");
    for (int64_t t = 1000000; t <= 10000000; t += s) (void)book.on_beat(t, 0, 0);
    expect(f.sent == 4 && f.net == 0 && book.algos_active() == 0, "t0 sell flat");

    // The leg that gives the position back is a close, not an opposite open.
    f = FakeExec{};
    f.net = 5;
    expect(book.algo_open(8, 9, static_cast<uint8_t>(pitlane::Side::Buy), 3, 20000000, s, 1000000, 0, 1) ==
               pitlane::kOpsOk,
           "t0 close legs open");
    for (int64_t t = 1000000; t <= 10000000; t += s) (void)book.on_beat(t, 0, 0);
    expect(f.sent == 6 && f.net == 5, "t0 close legs net");
    expect(f.off[0] == static_cast<uint8_t>(pitlane::Off::Open) &&
               f.off[5] == static_cast<uint8_t>(pitlane::Off::Close),
           "t0 opens then closes");

    // A basket that sells below the long position closes the difference.
    f = FakeExec{};
    f.net = 4;
    pitlane::OpsLeg legs[1]{};
    legs[0].name = 9;
    legs[0].target = 1;
    expect(book.basket_open(11, legs, 1, 1000000) == pitlane::kOpsOk, "basket close open");
    (void)book.on_beat(1000000, 0, 0);
    expect(f.sent == 1 && f.qty[0] == 3 && f.off[0] == static_cast<uint8_t>(pitlane::Off::Close),
           "basket close child");
    expect(f.net == 1, "basket close net");
    (void)book.on_beat(1500000, 0, 0);
    expect(book.baskets_active() == 0, "basket close retired");

    // A direct order carries the offset it was given and refuses an unknown one.
    f = FakeExec{};
    expect(book.direct(1, 9, static_cast<uint8_t>(pitlane::Side::Sell), 1, 0, 1000000, pitlane::Off::CloseToday) ==
               pitlane::kOpsOk,
           "direct close today");
    expect(f.off[0] == static_cast<uint8_t>(pitlane::Off::CloseToday), "direct close today offset");
    expect(book.direct(1, 9, static_cast<uint8_t>(pitlane::Side::Sell), 1, 0, 1000000,
                       static_cast<pitlane::Off>(9)) == pitlane::kOpsBadReq,
           "direct bad offset refused");
    f = FakeExec{};
    expect(book.direct(1, 9, static_cast<uint8_t>(pitlane::Side::Buy), 1, 100, 1000000, pitlane::Off::Open, nullptr,
                       pitlane::Biz::MarginBuy) == pitlane::kOpsOk,
           "direct margin buy");
    expect(f.biz[0] == static_cast<uint8_t>(pitlane::Biz::MarginBuy), "direct forwards the business");
    expect(sizeof(pitlane::Intent) == 56, "intent layout");

    // Running out of time while opening still unwinds what is held.
    f = FakeExec{};
    expect(book.algo_open(6, 9, static_cast<uint8_t>(pitlane::Side::Buy), 9, 2000000, s, 1000000, 0, 1) ==
               pitlane::kOpsOk,
           "t0 expiry open");
    for (int64_t t = 1000000; t <= 12000000; t += s) (void)book.on_beat(t, 0, 0);
    expect(f.net == 0 && book.algos_active() == 0, "t0 expiry flat");

    // A T0 needs the shared position feed.
    FakeExec g{};
    pitlane::ManualBook bare;
    bare.open(&mint, fake_send, &g, fake_top, nullptr);
    expect(bare.algo_open(6, 9, static_cast<uint8_t>(pitlane::Side::Buy), 1, h, s, 1000000, 0, 1) ==
               pitlane::kOpsBadReq,
           "t0 without net refused");
    expect(bare.algo_open(7, 9, static_cast<uint8_t>(pitlane::Side::Buy), 1, h, s, 1000000, 0, 2) ==
               pitlane::kOpsBadReq,
           "bad mode refused");
}

static void check_basket_capacity() {
    pitlane::BasketBook<2048> book;
    expect(book.open(7), "basket capacity open");
    bool filled = true;
    for (uint32_t name = 1; name <= 2000; ++name) {
        if (!book.put(name, 100)) {
            filled = false;
            break;
        }
    }
    expect(filled && book.legs() == 2000, "basket capacity legs");
    expect(book.find(1) == 0 && book.find(2000) == 1999 && book.find(2001) < 0, "basket capacity lookup");
}

static void check_ledger_feed() {
    pitlane::LedgerFeed feed;
    pitlane::LedgerAsset assets[2]{};
    for (int i = 0; i < 2; ++i) {
        assets[i].tier = static_cast<uint8_t>(pitlane::LedgerTier::Trader);
        assets[i].acct = static_cast<uint32_t>(i + 1);
        assets[i].dynamic_equity = 100000 + i;
        assets[i].available_cash = 60000 + i;
    }
    pitlane::LedgerPosition pos[3]{};
    for (int i = 0; i < 3; ++i) {
        pos[i].tier = static_cast<uint8_t>(pitlane::LedgerTier::Trader);
        pos[i].acct = 1;
        pos[i].name = 1;
        pos[i].direction = static_cast<uint8_t>(pitlane::LedgerSide::Long);
    }
    pos[1].direction = static_cast<uint8_t>(pitlane::LedgerSide::Short);
    pos[1].total = 3;
    pos[2].acct = 2;
    pos[2].total = 7;
    pos[0].total = 5;

    feed.replace(assets, 2, pos, 3, 1234);
    expect(feed.assets() == 2 && feed.positions() == 3, "ledger feed replace count");
    expect(feed.version() == 1 && feed.stamp_us() == 1234, "ledger feed stamp");

    pitlane::LedgerFilter f{};
    pitlane::LedgerAsset ao[8]{};
    pitlane::LedgerPosition po[8]{};
    int next = 0;
    bool last = false;
    int n = feed.query_assets(f, 0, ao, 8, &next, &last);
    expect(n == 2 && last && ao[1].acct == 2 && ao[1].dynamic_equity == 100001, "ledger feed assets");
    f.acct = 1;
    n = feed.query_positions(f, 0, po, 8, &next, &last);
    expect(n == 2 && last, "ledger feed acct filter");
    f.direction = static_cast<uint8_t>(pitlane::LedgerSide::Short);
    n = feed.query_positions(f, 0, po, 8, &next, &last);
    expect(n == 1 && po[0].total == 3, "ledger feed direction filter");
    f = pitlane::LedgerFilter{};
    n = feed.query_positions(f, 1, po, 1, &next, &last);
    expect(n == 1 && !last && next == 2, "ledger feed page");

    // Each half can be replaced on its own; a replace never appends.
    feed.replace_assets(assets, 1, 2000);
    expect(feed.assets() == 1 && feed.positions() == 3 && feed.version() == 2, "ledger feed replace assets");
    feed.replace_positions(pos, 0, 3000);
    expect(feed.positions() == 0 && feed.stamp_us() == 3000, "ledger feed clear positions");
    feed.clear();
    expect(feed.assets() == 0 && feed.positions() == 0, "ledger feed clear");
}

int main() {
    check_lots();
    check_ledger();
    check_ledger_view();
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
    check_prelock_credit_curve_wash();
    check_align_220();
    check_interleave_locks();
    check_pace_slice();
    check_pace_chase();
    check_pace_size();
    check_pace_book();
    check_flow_gate();
    check_precheck();
    check_link();
    check_exec_algo();
    check_exec_limit();
    check_auction();
    check_basket_ctl();
    check_rms_tick_lot();
    check_unlock();
    check_netting();
    check_pair_package();
    check_credit_book();
    check_manual_algo();
    check_basket_capacity();
    check_ledger_feed();
    if (g_fails) {
        std::fprintf(stderr, "%d order check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
