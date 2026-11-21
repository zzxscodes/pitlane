#include "pitlane/pitlane.hpp"

#include <cstdint>
#include <cstdio>

static int g_fails = 0;

static void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

static void feed_mid(pitlane::PairEngine& e, int64_t mid_a, int64_t mid_b, int64_t ts) {
    pitlane::PairTick a{};
    a.bid = mid_a - 1;
    a.ask = mid_a + 1;
    a.ts_us = ts;
    pitlane::PairTick b{};
    b.bid = mid_b - 1;
    b.ask = mid_b + 1;
    b.ts_us = ts;
    e.on_quote(0, a);
    e.on_quote(1, b);
}

static pitlane::PairCfg base_cfg() {
    pitlane::PairCfg c{};
    c.name_a = 7;
    c.name_b = 9;
    c.window = 8;
    c.entry_z_ppm = 1500000;
    c.exit_z_ppm = 300000;
    c.stop_z_ppm = 6000000;
    c.qty_a = 2;
    c.ratio_ppm = 1000000;
    c.cooldown_us = 0;
    c.leg_timeout_us = 1000000;
    c.max_stale_us = 0;
    c.max_hold_us = 60000000;
    return c;
}

void check_config() {
    pitlane::PairEngine e;
    pitlane::PairCfg c = base_cfg();
    expect(e.open(c), "open default");
    expect(e.is_open() && e.state() == pitlane::kPairFlat, "initial state");

    c = base_cfg();
    c.name_b = c.name_a;
    expect(!e.open(c), "reject same names");

    c = base_cfg();
    c.entry_z_ppm = 0;
    expect(!e.open(c), "reject zero entry");

    c = base_cfg();
    c.window = 1;
    expect(!e.open(c), "reject short window");

    c = base_cfg();
    c.stop_z_ppm = 100;
    expect(!e.open(c), "reject stop below entry");

    c = base_cfg();
    c.ratio_ppm = 0;
    expect(!e.open(c), "reject zero ratio");
}

void check_round_trip() {
    pitlane::PairEngine e;
    expect(e.open(base_cfg()), "open round trip");

    int64_t ts = 1000;
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, ts);
        const bool ready = e.update(ts);
        if (i == 7) expect(ready, "flat samples ready");
        ts += 1000;
    }
    expect(e.row().z_ppm == 0, "flat spread z");

    feed_mid(e, 21000, 20000, ts);
    e.update(ts);
    pitlane::PairRow row = e.row();
    expect(row.ready == 1 && row.z_ppm > 1500000, "entry z");

    pitlane::PairReq req[2]{};
    int n = e.step(ts, req, 2);
    expect(n == 2, "two entry legs");
    expect(req[0].leg == 0 && req[0].side == 2 && req[0].qty == 2, "sell A leg");
    expect(req[1].leg == 1 && req[1].side == 1 && req[1].qty == 2, "buy B leg");
    expect(req[0].px == 20999 && req[1].px == 20001, "entry at touch");
    expect(e.state() == pitlane::kPairOpening, "opening state");

    e.on_fill(0, 2, 2, req[0].px);
    e.on_fill(1, 1, 2, req[1].px);
    n = e.step(ts, req, 2);
    expect(n == 0 && e.state() == pitlane::kPairHolding, "holding after fills");

    ts += 1000;
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, ts);
        e.update(ts);
        ts += 1000;
    }
    row = e.row();
    expect(row.z_ppm <= 300000 && row.z_ppm >= -300000, "exit z");

    n = e.step(ts, req, 2);
    expect(n == 2 && e.state() == pitlane::kPairClosing, "closing legs");
    expect(req[0].side == 1 && req[0].px == 20001, "buy back A");
    expect(req[1].side == 2 && req[1].px == 19999, "sell B");

    e.on_fill(0, 1, 2, req[0].px);
    e.on_fill(1, 2, 2, req[1].px);
    ts += 1000;
    e.step(ts, req, 2);
    row = e.row();
    expect(row.state == pitlane::kPairFlat && row.rounds == 1 && row.wins == 1, "settled round");
    expect(row.realized == 1992, "realized spread pnl");
    expect(row.pos_a == 0 && row.pos_b == 0, "flat positions");
    expect(row.signals == 1 && row.orders == 4, "signal and order counters");
}

void check_ratio_and_stop() {
    pitlane::PairCfg c = base_cfg();
    c.qty_a = 3;
    c.ratio_ppm = 500000;
    c.entry_z_ppm = 1000000;
    c.exit_z_ppm = 200000;
    c.stop_z_ppm = 1700000;
    pitlane::PairEngine e;
    expect(e.open(c), "open ratio");

    int64_t ts = 1000;
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, ts);
        e.update(ts);
        ts += 1000;
    }
    feed_mid(e, 21000, 20000, ts);
    e.update(ts);
    pitlane::PairReq req[2]{};
    expect(e.step(ts, req, 2) == 2, "ratio entry");
    expect(req[0].qty == 3 && req[1].qty == 1, "hedge ratio rounds down to one lot");
    e.on_fill(0, 2, 3, req[0].px);
    e.on_fill(1, 1, 1, req[1].px);
    e.step(ts, req, 2);
    expect(e.state() == pitlane::kPairHolding, "ratio holding");

    ts += 1000;
    feed_mid(e, 21000, 20000, ts);
    e.update(ts);
    expect(e.row().stop_hits == 0, "no stop yet");
    expect(e.step(ts, req, 2) == 2, "stop close legs");
    expect(e.row().stop_hits == 1 && e.state() == pitlane::kPairClosing, "stop triggered");
    e.on_fill(0, 1, 3, req[0].px);
    e.on_fill(1, 2, 1, req[1].px);
    ts += 1000;
    e.step(ts, req, 2);
    const pitlane::PairRow row = e.row();
    expect(row.state == pitlane::kPairFlat && row.rounds == 1 && row.losses == 1, "stop settles a loss");
    expect(row.realized < 0, "negative stop pnl");
}

void check_naked_leg_timeout() {
    pitlane::PairCfg c = base_cfg();
    c.entry_z_ppm = 1000000;
    c.leg_timeout_us = 5000;
    pitlane::PairEngine e;
    expect(e.open(c), "open naked");

    int64_t ts = 1000;
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, ts);
        e.update(ts);
        ts += 1000;
    }
    feed_mid(e, 21500, 20000, ts);
    e.update(ts);
    pitlane::PairReq req[2]{};
    expect(e.step(ts, req, 2) == 2, "naked entry");
    e.on_fill(0, 2, 2, req[0].px);

    ts += 6000;
    const int n = e.step(ts, req, 2);
    expect(n == 1 && req[0].leg == 0 && req[0].side == 1 && req[0].qty == 2, "unwind filled leg");
    const pitlane::PairRow row = e.row();
    expect(row.state == pitlane::kPairClosing && row.timeouts == 1, "naked timeout closing");
}

void check_cooldown() {
    pitlane::PairCfg c = base_cfg();
    c.cooldown_us = 1000000;
    c.exit_z_ppm = 300000;
    pitlane::PairEngine e;
    expect(e.open(c), "open cooldown");

    int64_t ts = 1000;
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, ts);
        e.update(ts);
        ts += 1000;
    }
    feed_mid(e, 21000, 20000, ts);
    e.update(ts);
    pitlane::PairReq req[2]{};
    expect(e.step(ts, req, 2) == 2, "cooldown entry");
    e.on_fill(0, 2, 2, req[0].px);
    e.on_fill(1, 1, 2, req[1].px);
    e.step(ts, req, 2);

    ts += 1000;
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, ts);
        e.update(ts);
        ts += 1000;
    }
    expect(e.step(ts, req, 2) == 2, "cooldown close");
    e.on_fill(0, 1, 2, req[0].px);
    e.on_fill(1, 2, 2, req[1].px);
    ts += 1000;
    e.step(ts, req, 2);
    expect(e.state() == pitlane::kPairFlat, "cooldown flat");

    // 立刻再次出现开仓信号，冷却期内不允许开仓。
    feed_mid(e, 21000, 20000, ts);
    e.update(ts);
    expect(e.step(ts, req, 2) == 0 && e.state() == pitlane::kPairFlat, "cooldown blocks entry");

    ts += 1200000;
    e.step(ts, req, 2);
    expect(e.state() == pitlane::kPairOpening, "cooldown expires");
}

void check_sync_position() {
    pitlane::PairCfg c = base_cfg();
    c.fee_ppm = 10000;
    pitlane::PairEngine e;
    expect(e.open(c), "open sync");

    int64_t ts = 1000;
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, ts);
        e.update(ts);
        ts += 1000;
    }
    feed_mid(e, 21000, 20000, ts);
    e.update(ts);
    pitlane::PairReq req[2]{};
    expect(e.step(ts, req, 2) == 2, "sync entry");
    e.sync_position(0, -2);
    e.sync_position(1, 2);
    e.step(ts, req, 2);
    expect(e.state() == pitlane::kPairHolding, "sync holding");

    ts += 1000;
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, ts);
        e.update(ts);
        ts += 1000;
    }
    expect(e.step(ts, req, 2) == 2, "sync close");
    e.sync_position(0, 0);
    e.sync_position(1, 0);
    ts += 1000;
    e.step(ts, req, 2);
    const pitlane::PairRow row = e.row();
    expect(row.state == pitlane::kPairFlat && row.rounds == 1, "sync settled");
    // 4 笔成交，每笔名义金额约 4 万，手续费 1%% 后应明显小于毛价差。
    expect(row.realized > 0 && row.realized < 1992, "fees reduce pnl");
}

void check_skew_guard() {
    pitlane::PairCfg c = base_cfg();
    c.max_skew_us = 500000;
    pitlane::PairEngine e;
    expect(e.open(c), "open skew");

    int64_t ts = 1000;
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, ts);
        e.update(ts);
        ts += 1000;
    }
    expect(e.row().ready && e.row().spread == 0, "skew baseline");

    // A 腿单独前进 2 秒，超过两腿时间差上限，本拍不得产生价差样本。
    pitlane::PairTick a{};
    a.bid = 20999;
    a.ask = 21001;
    a.ts_us = ts + 2000000;
    e.on_quote(0, a);
    expect(e.update(ts + 2000000), "skew keeps ready");
    expect(e.row().spread == 0, "skew skips sample");

    // B 腿追平时间戳后，价差才计入窗口。
    pitlane::PairTick b{};
    b.bid = 19999;
    b.ask = 20001;
    b.ts_us = ts + 2000000;
    e.on_quote(1, b);
    e.update(ts + 2000000);
    expect(e.row().spread == 1000, "aligned sample");
    expect(e.row().z_ppm > 0, "aligned z positive");
}

void check_stale_guard() {
    pitlane::PairCfg c = base_cfg();
    c.max_stale_us = 2000000;
    c.leg_timeout_us = 1000000;
    pitlane::PairEngine e;
    expect(e.open(c), "open stale");

    int64_t ts = 1000;
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, ts);
        e.update(ts);
        ts += 1000;
    }
    feed_mid(e, 21000, 20000, ts);
    e.update(ts);
    pitlane::PairReq req[2]{};
    expect(e.step(ts, req, 2) == 2, "stale entry");
    e.on_fill(0, 2, 2, req[0].px);
    e.on_fill(1, 1, 2, req[1].px);
    e.step(ts, req, 2);
    expect(e.state() == pitlane::kPairHolding, "stale holding");

    // 行情断流超过过期窗口：不报单、不计超时，状态保持。
    const pitlane::PairRow before = e.row();
    int64_t far = ts + 3000000;
    expect(e.step(far, req, 2) == 0, "stale emits nothing");
    const pitlane::PairRow after = e.row();
    expect(after.orders == before.orders && after.timeouts == before.timeouts, "stale counters frozen");
    expect(after.state == pitlane::kPairHolding, "stale keeps state");

    // 行情恢复后继续按价差平仓。
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, far);
        e.update(far);
        far += 1000;
    }
    expect(e.step(far, req, 2) == 2, "stale recovery closes");
}

static void feed_book(pitlane::PairEngine& e, uint8_t leg, int64_t bid, int64_t ask, int64_t ts) {
    pitlane::PairTick t{};
    t.bid = bid;
    t.ask = ask;
    t.ts_us = ts;
    e.on_quote(leg, t);
}

static void warm_window(pitlane::PairEngine& e, int64_t& ts) {
    for (int i = 0; i < 8; ++i) {
        feed_mid(e, 20000, 20000, ts);
        e.update(ts);
        ts += 1000;
    }
}

void check_controls() {
    pitlane::PairCfg c = base_cfg();
    c.entry_z_ppm = 1000000;
    pitlane::PairEngine e;
    expect(e.open(c), "open controls");
    int64_t ts = 1000;
    warm_window(e, ts);
    feed_mid(e, 21000, 20000, ts);
    e.update(ts);

    pitlane::PairReq req[2]{};
    expect(e.pause(), "pause accepted");
    expect(!e.pause(), "double pause rejected");
    expect(e.step(ts, req, 2) == 0 && e.state() == pitlane::kPairFlat, "pause blocks entry");
    expect(e.resume(), "resume accepted");
    expect(!e.resume(), "double resume rejected");
    expect(e.step(ts, req, 2) == 2, "resume emits entry");
    expect(e.stop(), "stop accepted");
    expect(e.stopped() && e.step(ts, req, 2) == 0, "stop blocks step");
    expect(!e.stop(), "double stop rejected");
}

void check_pending_leg() {
    pitlane::PairCfg c = base_cfg();
    c.pending_ppm = 500000;
    c.main_leg = 0;
    c.pending_modify_off = 4;
    c.pending_cancel_off = 9;
    pitlane::PairEngine e;
    expect(e.open(c), "open pending leg");
    int64_t ts = 1000;
    warm_window(e, ts);

    // 盘口拉开 10 个价位，价差走高：卖 A 主动，买 B 被动。
    feed_book(e, 0, 21995, 22005, ts);
    feed_book(e, 1, 19995, 20005, ts);
    e.update(ts);
    pitlane::PairReq req[2]{};
    expect(e.step(ts, req, 2) == 2, "pending entry legs");
    expect(req[0].leg == 0 && req[0].side == 2 && req[0].px == 21995, "hitter sells at the bid");
    expect(req[1].leg == 1 && req[1].side == 1 && req[1].px == 20000, "pending buys inside the spread");

    // B 盘口下移 5 个价位：被动腿改价。
    feed_book(e, 1, 19990, 20000, ts + 1000);
    int64_t px = 0;
    expect(e.pending_action(1, px) == pitlane::kPendingReprice && px == 19995, "pending reprice");
    e.on_amend(1, px);
    expect(e.pending_action(1, px) == pitlane::kPendingKeep, "amended price holds");

    // 再下移 10 个价位：超过撤单阈值，撤单后按新价重发剩余差额。
    feed_book(e, 1, 19980, 19990, ts + 2000);
    expect(e.pending_action(1, px) == pitlane::kPendingCancel && px == 19985, "pending cancel");
    e.on_cancel(1);
    expect(e.pending_action(1, px) == pitlane::kPendingKeep, "cancelled pending clears");
    expect(e.step(ts + 2000, req, 2) == 1 && req[0].leg == 1 && req[0].px == 19985,
           "pending re-emits after cancel");
}

void check_sequential() {
    pitlane::PairCfg c = base_cfg();
    c.sequential = true;
    c.main_leg = 0;
    pitlane::PairEngine e;
    expect(e.open(c), "open sequential");
    int64_t ts = 1000;
    warm_window(e, ts);
    feed_mid(e, 21000, 20000, ts);
    e.update(ts);

    pitlane::PairReq req[2]{};
    expect(e.step(ts, req, 2) == 1 && req[0].leg == 0, "sequential main leg first");
    e.on_fill(0, 2, 2, req[0].px);
    expect(e.step(ts, req, 2) == 1 && req[0].leg == 1, "sequential hedge leg after main");
    e.on_fill(1, 1, 2, req[0].px);
    e.step(ts, req, 2);
    expect(e.state() == pitlane::kPairHolding, "sequential holding");
}

void check_max_leg_qty() {
    pitlane::PairCfg c = base_cfg();
    c.qty_a = 5;
    c.max_leg_qty = 2;
    c.entry_z_ppm = 1000000;
    pitlane::PairEngine e;
    expect(e.open(c), "open leg cap");
    int64_t ts = 1000;
    warm_window(e, ts);
    feed_mid(e, 21500, 20000, ts);
    e.update(ts);

    pitlane::PairReq req[2]{};
    expect(e.step(ts, req, 2) == 2, "capped entry legs");
    expect(req[0].qty == 2 && req[1].qty == 2, "each leg capped");
    e.on_fill(0, 2, 2, req[0].px);
    e.on_fill(1, 1, 2, req[1].px);
    expect(e.step(ts, req, 2) == 2 && req[0].qty == 2 && req[1].qty == 2, "next slice capped");
}

void check_unack_expiry() {
    pitlane::PairCfg c = base_cfg();
    c.entry_z_ppm = 1000000;
    c.ack_timeout_us = 100000;
    pitlane::PairEngine e;
    expect(e.open(c), "open unack");
    int64_t ts = 1000;
    warm_window(e, ts);
    feed_mid(e, 21500, 20000, ts);
    e.update(ts);

    pitlane::PairReq req[2]{};
    expect(e.step(ts, req, 2) == 2, "unack entry");
    expect(!e.unack_expired(0, ts + 50000), "unack young");
    expect(e.unack_expired(0, ts + 150000), "unack expiry");
    e.on_cancel(0);
    expect(!e.unack_expired(0, ts + 150000), "cancel clears unack");
    expect(e.step(ts + 150000, req, 2) == 1 && req[0].leg == 0, "unacked leg re-emits");
}

void check_combo_spread() {
    pitlane::PairCfg c = base_cfg();
    c.combo_check = true;
    c.entry_z_ppm = 100000;
    pitlane::PairEngine e;
    expect(e.open(c), "open combo");
    int64_t ts = 1000;
    warm_window(e, ts);
    feed_book(e, 0, 20999, 21001, ts);
    feed_book(e, 1, 19999, 20001, ts);
    e.update(ts);

    expect(e.combo_spread(-1) == 998 && e.combo_spread(1) == 1002, "combo touch prices");
    expect(e.combo_ok(-1) && !e.combo_ok(1), "combo keeps only the signal side");
    // A 腿买价掉到 20100：卖出 A 买进 B 的组合价差只剩 99，低于入场带边界。
    feed_book(e, 0, 20100, 21002, ts);
    expect(!e.combo_ok(-1), "combo loses the edge");
}

int main() {
    check_config();
    check_round_trip();
    check_ratio_and_stop();
    check_naked_leg_timeout();
    check_cooldown();
    check_sync_position();
    check_skew_guard();
    check_stale_guard();
    check_controls();
    check_pending_leg();
    check_sequential();
    check_max_leg_qty();
    check_unack_expiry();
    check_combo_spread();
    if (g_fails != 0) {
        std::fprintf(stderr, "%d strat checks failed\n", g_fails);
        return 1;
    }
    std::printf("strat checks ok\n");
    return 0;
}
