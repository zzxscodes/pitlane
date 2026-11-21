// Covers the C ABI in include/pitlane/capi/pitlane_c.h: tape write/read,
// metadata, column projection, stats, and the single-name backtest.

#include "pitlane/capi/pitlane_c.h"

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

static constexpr uint32_t kN = 10000;
static constexpr uint32_t kSymbol = 7;

static void make_ticks(pl_tick* t, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const int64_t trend = i < n / 2 ? static_cast<int64_t>(i) : static_cast<int64_t>(n - i);
        const int64_t px = 1000000 + trend;
        t[i] = pl_tick{};
        t[i].exch_us = 1000000 + static_cast<int64_t>(i) * 1000;
        t[i].local_us = t[i].exch_us + 7;
        t[i].bid_px = px - 100;
        t[i].ask_px = px + 100;
        t[i].last_px = px;
        t[i].bid_qty = 10 + static_cast<int64_t>(i % 5);
        t[i].ask_qty = 12 + static_cast<int64_t>(i % 3);
        t[i].last_qty = 1 + static_cast<int64_t>(i % 7);
    }
}

static void check_bridge() {
    expect(pl_now_us() > 0, "bridge clock");
    pl_bridge* b = pl_bridge_open(4, 2);
    expect(b != nullptr, "bridge open");
    if (!b) return;

    pl_event ev{};
    ev.kind = PL_EV_QUOTE;
    ev.name = kSymbol;
    ev.px = 100;
    ev.qty = 3;
    ev.stamp_us = 123;
    ev.seq = 1;
    expect(pl_bridge_post(b, &ev) == 1 && pl_bridge_events_pending(b) == 1, "bridge post");
    ev.kind = PL_EV_TRADE;
    ev.seq = 2;
    expect(pl_bridge_post(b, &ev) == 1, "bridge post second");

    pl_event out[4]{};
    uint32_t n = 99;
    expect(pl_bridge_poll(b, out, 4, &n) == 1 && n == 2 && out[0].kind == PL_EV_QUOTE && out[1].seq == 2,
           "bridge poll order");
    expect(pl_bridge_poll(b, out, 4, &n) == 1 && n == 0 && pl_bridge_events_pending(b) == 0, "bridge poll empty");

    for (uint16_t i = 0; i < 4; ++i) {
        ev.seq = static_cast<uint16_t>(10 + i);
        expect(pl_bridge_post(b, &ev) == 1, "bridge fill");
    }
    expect(pl_bridge_post(b, &ev) == 0 && pl_bridge_events_lost(b) == 1, "bridge full drops");
    expect(pl_bridge_poll(b, out, 3, &n) == 1 && n == 3 && out[0].seq == 10 && out[2].seq == 12, "bridge partial");
    expect(pl_bridge_poll(b, out, 3, &n) == 1 && n == 1 && out[0].seq == 13, "bridge wrap");

    pl_cmd cmd{};
    cmd.kind = PL_CMD_INSERT;
    cmd.id = 42;
    cmd.name = kSymbol;
    cmd.px = 101;
    cmd.qty = 1;
    cmd.side = 1;
    expect(pl_bridge_send(b, &cmd) == 1 && pl_bridge_cmds_pending(b) == 1, "bridge send");
    pl_cmd got{};
    expect(pl_bridge_take_cmd(b, &got) == 1 && got.id == 42 && got.px == 101 && got.side == 1, "bridge take");
    expect(pl_bridge_take_cmd(b, &got) == 0, "bridge take empty");
    expect(pl_bridge_send(b, &cmd) == 1 && pl_bridge_send(b, &cmd) == 1, "bridge cmd fill");
    expect(pl_bridge_send(b, &cmd) == 0 && pl_bridge_cmds_lost(b) == 1, "bridge cmd full drops");

    expect(pl_bridge_log(b, 2, "hello") == 1 && pl_bridge_logs_pending(b) == 1, "bridge log");
    pl_log line{};
    expect(pl_bridge_take_log(b, &line) == 1 && line.level == 2 && std::strcmp(line.text, "hello") == 0 &&
               line.stamp_us > 0,
           "bridge log take");
    expect(pl_bridge_take_log(b, &line) == 0, "bridge log empty");
    char wide[PL_LOG_TEXT + 64];
    for (std::size_t i = 0; i < sizeof wide - 1; ++i) wide[i] = 'x';
    wide[sizeof wide - 1] = 0;
    expect(pl_bridge_log(b, 4, wide) == 1, "bridge log wide");
    expect(pl_bridge_take_log(b, &line) == 1 && std::strlen(line.text) == PL_LOG_TEXT - 1, "bridge log truncates");
    for (uint32_t i = 0; i < 1024; ++i) expect(pl_bridge_log(b, 2, "f") == 1, "bridge log fill");
    expect(pl_bridge_log(b, 2, "f") == 0 && pl_bridge_logs_lost(b) == 1, "bridge log full drops");

    expect(pl_bridge_post(nullptr, &ev) == 0 && pl_bridge_post(b, nullptr) == 0, "bridge post guards");
    expect(pl_bridge_poll(b, out, 4, nullptr) == 0 && pl_bridge_poll(nullptr, out, 4, &n) == 0, "bridge poll guards");
    expect(pl_bridge_send(b, nullptr) == 0 && pl_bridge_take_cmd(b, nullptr) == 0, "bridge cmd guards");
    expect(pl_bridge_log(nullptr, 2, "x") == 0 && pl_bridge_take_log(b, nullptr) == 0, "bridge log guards");
    expect(pl_bridge_events_pending(nullptr) == 0 && pl_bridge_cmds_pending(nullptr) == 0 &&
               pl_bridge_events_lost(nullptr) == 0 && pl_bridge_cmds_lost(nullptr) == 0 &&
               pl_bridge_logs_pending(nullptr) == 0 && pl_bridge_logs_lost(nullptr) == 0,
           "bridge null pending");
    pl_bridge_close(b);
    pl_bridge_close(nullptr);
}

int main() {
    check_bridge();
    char path[128];
    std::snprintf(path, sizeof path, "/tmp/pitlane_capi_%ld.plt", static_cast<long>(::getpid()));

    expect(pl_tape_open("/tmp/pitlane_capi_missing.plt") == nullptr, "missing tape opens NULL");

    static pl_tick ticks[kN];
    make_ticks(ticks, kN);
    expect(pl_tape_write(path, kSymbol, ticks, kN) == 1, "write tape");

    pl_tape* t = pl_tape_open(path);
    expect(t != nullptr, "open tape");
    if (t) {
        pl_meta meta{};
        expect(pl_tape_meta(t, &meta) == 1, "meta");
        expect(meta.versions == 1 && meta.rows == kN && meta.symbol == kSymbol, "meta fields");
        expect(meta.blocks == 10, "meta blocks");
        expect(meta.first_ts == ticks[0].exch_us && meta.last_ts == ticks[kN - 1].exch_us, "meta ts range");
        expect(meta.bytes > 0, "meta bytes");

        pl_query q{};
        q.version = 0;
        q.since_us = INT64_MIN;
        q.until_us = INT64_MAX;
        q.cols = PL_TAPE_COLUMNS == 8 ? 0xff : 0;
        pl_tick out[16];
        uint32_t n = 0;
        expect(pl_tape_rows(t, &q, out, 16, &n) == 1 && n == 16, "rows default");
        expect(out[0].exch_us == ticks[0].exch_us && out[0].bid_px == ticks[0].bid_px, "rows first");
        expect(out[15].exch_us == ticks[15].exch_us, "rows last");

        q.limit = 3;
        expect(pl_tape_rows(t, &q, out, 16, &n) == 1 && n == 3, "rows limit");
        q.limit = 0;
        q.offset = kN - 2;
        expect(pl_tape_rows(t, &q, out, 16, &n) == 1 && n == 2 && out[1].exch_us == ticks[kN - 1].exch_us,
               "rows offset");

        pl_stats st{};
        expect(pl_tape_describe(t, 0, &st) == 1, "describe");
        expect(st.version == 1 && st.rows == kN, "describe counts");
        int64_t bid_min = ticks[0].bid_px;
        int64_t bid_max = ticks[0].bid_px;
        for (uint32_t i = 1; i < kN; ++i) {
            if (ticks[i].bid_px < bid_min) bid_min = ticks[i].bid_px;
            if (ticks[i].bid_px > bid_max) bid_max = ticks[i].bid_px;
        }
        expect(st.min[2] == bid_min && st.max[2] == bid_max, "describe bid range");
        expect(st.mean[2] > 0, "describe bid mean");

        pl_bt_cfg cfg{};
        cfg.cash = 1000000;
        cfg.qty = 1;
        cfg.fee_ppm = 20;
        cfg.min_fee = 0;
        cfg.slip_ticks = 0;
        cfg.tick = 1;
        cfg.threshold_ppm = 1;
        cfg.cooldown_us = 0;
        cfg.sign = 1;
        cfg.kind = 2;
        cfg.window = 8;
        cfg.stride = 1;
        pl_bt_metrics m{};
        expect(pl_backtest(path, &cfg, &m) == 1, "backtest");
        expect(m.points >= 2, "backtest points");
        expect(m.trades >= 0, "backtest trades");

        expect(pl_backtest(path, nullptr, &m) == 0, "backtest rejects null cfg");
        expect(pl_backtest(path, &cfg, nullptr) == 0, "backtest rejects null metrics");
        expect(pl_tape_meta(nullptr, &meta) == 0, "meta rejects null tape");
        expect(pl_tape_rows(t, &q, nullptr, 16, &n) == 0, "rows rejects null out");

        pl_tape_close(t);
    }

    const char* version = pl_version();
    expect(version != nullptr && version[0] != '\0' && std::strcmp(version, "unknown") != 0, "version string");

    ::unlink(path);
    if (g_fails) {
        std::fprintf(stderr, "%d capi check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
