// C ABI implementation over the cold-path study tools. Kept deliberately thin:
// no exceptions, no RTTI; heap use is confined to pl_tape_open and pl_backtest.
// Every entry point validates its arguments and turns any failure into 0 / NULL.

#include "pitlane/capi/pitlane_c.h"

#include "pitlane/study/backtest.hpp"
#include "pitlane/study/block_tape.hpp"
#include "pitlane/desk/types.hpp"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <new>
#include <vector>

#ifndef PITLANE_VERSION_STR
#define PITLANE_VERSION_STR "unknown"
#endif

struct pl_tape {
    pitlane::BlockTapeReader reader;
};

// Two single-producer/single-consumer rings, plus a strategy-side log ring.
// Each side owns one cursor: the engine writes events and reads commands, the
// strategy does the opposite. The log ring runs strategy -> engine.
struct pl_bridge {
    pl_event* events{nullptr};
    pl_cmd* cmds{nullptr};
    pl_log* logs{nullptr};
    uint32_t event_mask{0};
    uint32_t cmd_mask{0};
    uint32_t log_mask{0};
    alignas(64) std::atomic<uint32_t> ev_head{0};
    alignas(64) std::atomic<uint32_t> ev_tail{0};
    alignas(64) std::atomic<uint32_t> cmd_head{0};
    alignas(64) std::atomic<uint32_t> cmd_tail{0};
    alignas(64) std::atomic<uint32_t> log_head{0};
    alignas(64) std::atomic<uint32_t> log_tail{0};
    alignas(64) std::atomic<uint64_t> ev_lost{0};
    alignas(64) std::atomic<uint64_t> cmd_lost{0};
    alignas(64) std::atomic<uint64_t> log_lost{0};
};

namespace {

constexpr uint32_t kWriteChunk = 1024;
constexpr uint32_t kReadChunk = 512;
constexpr uint32_t kBacktestCap = 1u << 16;
constexpr uint32_t kDefaultEvents = 1u << 12;
constexpr uint32_t kDefaultCmds = 1u << 10;
constexpr uint32_t kMaxRing = 1u << 20;
constexpr uint32_t kLogCap = 1024;

int64_t mono_us() noexcept {
    timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

uint32_t ring_size(uint32_t want, uint32_t fallback) noexcept {
    uint32_t n = want ? want : fallback;
    if (n > kMaxRing) n = kMaxRing;
    uint32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

void copy_tick(const pl_tick& src, pitlane::TapeTick& dst) noexcept {
    dst.exch_us = src.exch_us;
    dst.local_us = src.local_us;
    dst.bid_px = src.bid_px;
    dst.ask_px = src.ask_px;
    dst.last_px = src.last_px;
    dst.bid_qty = src.bid_qty;
    dst.ask_qty = src.ask_qty;
    dst.last_qty = src.last_qty;
}

} // namespace

extern "C" {

pl_tape* pl_tape_open(const char* path) {
    if (!path || !path[0]) return nullptr;
    pl_tape* t = new (std::nothrow) pl_tape();
    if (!t) return nullptr;
    if (!t->reader.open(path)) {
        delete t;
        return nullptr;
    }
    return t;
}

void pl_tape_close(pl_tape* t) { delete t; }

int pl_tape_meta(const pl_tape* t, pl_meta* out) {
    if (!t || !out) return 0;
    pitlane::TapeMeta m{};
    if (!t->reader.meta(m)) return 0;
    out->versions = m.versions;
    out->blocks = m.blocks;
    out->symbol = m.symbol;
    out->rows = m.rows;
    out->bytes = m.bytes;
    out->first_ts = m.first_ts;
    out->last_ts = m.last_ts;
    return 1;
}

int pl_tape_rows(pl_tape* t, const pl_query* q, pl_tick* out, uint32_t cap, uint32_t* n) {
    if (n) *n = 0;
    if (!t || !q || !out || !n || cap == 0) return 0;
    const uint32_t total = (q->limit == 0 || q->limit > cap) ? cap : q->limit;
    pitlane::TapeTick buf[kReadChunk];
    uint32_t written = 0;
    // The reader exposes TapeTick; page through it in fixed chunks so the C
    // struct is written field by field and never aliased.
    while (written < total) {
        if (written > 0xffffffffu - q->offset) break;
        uint32_t want = total - written;
        if (want > kReadChunk) want = kReadChunk;
        uint32_t got = 0;
        if (!t->reader.rows(q->version, q->since_us, q->until_us, q->offset + written, want, q->cols, buf, want, got)) {
            return 0;
        }
        if (got == 0) break;
        for (uint32_t i = 0; i < got; ++i) {
            const pitlane::TapeTick& s = buf[i];
            out[written + i] = pl_tick{s.exch_us, s.local_us, s.bid_px, s.ask_px,
                                       s.last_px, s.bid_qty, s.ask_qty, s.last_qty};
        }
        written += got;
        if (got < want) break;
    }
    *n = written;
    return 1;
}

int pl_tape_describe(pl_tape* t, uint32_t version, pl_stats* out) {
    if (!t || !out) return 0;
    pitlane::TapeStats st{};
    if (!t->reader.describe(version, st)) return 0;
    out->version = st.version;
    out->pad = 0;
    out->rows = st.rows;
    for (uint32_t c = 0; c < PL_TAPE_COLUMNS; ++c) {
        out->min[c] = st.col[c].min;
        out->max[c] = st.col[c].max;
        out->last[c] = st.col[c].last;
        out->mean[c] = st.col[c].mean;
    }
    return 1;
}

int pl_tape_write(const char* path, uint32_t symbol, const pl_tick* ticks, uint32_t n) {
    if (!path || !path[0] || !ticks || n == 0) return 0;
    pitlane::BlockTapeWriter w;
    if (!w.open(path, symbol)) return 0;
    pitlane::TapeTick buf[kWriteChunk];
    uint32_t done = 0;
    while (done < n) {
        uint32_t take = n - done;
        if (take > kWriteChunk) take = kWriteChunk;
        for (uint32_t i = 0; i < take; ++i) copy_tick(ticks[done + i], buf[i]);
        if (!w.append(buf, static_cast<int>(take))) return 0;
        done += take;
    }
    return w.commit() ? 1 : 0;
}

int pl_backtest(const char* path, const pl_bt_cfg* cfg, pl_bt_metrics* out) {
    if (!path || !path[0] || !cfg || !out) return 0;
    pitlane::BlockTapeReader r;
    if (!r.open(path)) return 0;
    std::vector<pitlane::Quote> quotes(kBacktestCap);
    uint32_t n = 0;
    if (!r.load_quotes(0, quotes.data(), kBacktestCap, n) || n < 2) return 0;
    quotes.resize(n);

    pitlane::BtCfg bc{};
    bc.cash = cfg->cash;
    bc.qty = cfg->qty;
    bc.fee_ppm = cfg->fee_ppm;
    bc.min_fee = cfg->min_fee;
    bc.slip_ticks = cfg->slip_ticks;
    bc.tick = cfg->tick;
    bc.threshold_ppm = cfg->threshold_ppm;
    bc.cooldown_us = cfg->cooldown_us;
    bc.sign = cfg->sign;
    bc.kind = cfg->kind;
    bc.window = cfg->window;
    bc.stride = cfg->stride;

    pitlane::BtMetrics m{};
    pitlane::Backtester<1024> bt;
    if (!bt.run(quotes.data(), static_cast<int>(n), bc, m)) return 0;

    out->points = m.points;
    out->trades = m.trades;
    out->round_trips = m.round_trips;
    out->wins = m.wins;
    out->gross_pnl = m.gross_pnl;
    out->fees = m.fees;
    out->net_pnl = m.net_pnl;
    out->turnover = m.turnover;
    out->return_ppm = m.return_ppm;
    out->annual_ppm = m.annual_ppm;
    out->sharpe_ppm = m.sharpe_ppm;
    out->max_dd_ppm = m.max_dd_ppm;
    out->win_ppm = m.win_ppm;
    out->exposure_ppm = m.exposure_ppm;
    out->avg_trade_pnl = m.avg_trade_pnl;
    out->span_us = m.span_us;
    out->final_equity = m.final_equity;
    out->peak_equity = m.peak_equity;
    return 1;
}

pl_bridge* pl_bridge_open(uint32_t event_cap, uint32_t cmd_cap) {
    const uint32_t ne = ring_size(event_cap, kDefaultEvents);
    const uint32_t nc = ring_size(cmd_cap, kDefaultCmds);
    pl_bridge* b = new (std::nothrow) pl_bridge();
    if (!b) return nullptr;
    b->events = new (std::nothrow) pl_event[ne];
    b->cmds = new (std::nothrow) pl_cmd[nc];
    b->logs = new (std::nothrow) pl_log[kLogCap];
    if (!b->events || !b->cmds || !b->logs) {
        delete[] b->events;
        delete[] b->cmds;
        delete[] b->logs;
        delete b;
        return nullptr;
    }
    b->event_mask = ne - 1;
    b->cmd_mask = nc - 1;
    b->log_mask = kLogCap - 1;
    return b;
}

void pl_bridge_close(pl_bridge* b) {
    if (!b) return;
    delete[] b->events;
    delete[] b->cmds;
    delete[] b->logs;
    delete b;
}

int pl_bridge_post(pl_bridge* b, const pl_event* ev) {
    if (!b || !ev) return 0;
    const uint32_t head = b->ev_head.load(std::memory_order_relaxed);
    const uint32_t tail = b->ev_tail.load(std::memory_order_acquire);
    if (head - tail > b->event_mask) {
        b->ev_lost.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    b->events[head & b->event_mask] = *ev;
    b->ev_head.store(head + 1, std::memory_order_release);
    return 1;
}

int pl_bridge_poll(pl_bridge* b, pl_event* out, uint32_t cap, uint32_t* n) {
    if (n) *n = 0;
    if (!b || !out || !n) return 0;
    const uint32_t head = b->ev_head.load(std::memory_order_acquire);
    const uint32_t tail = b->ev_tail.load(std::memory_order_relaxed);
    uint32_t take = head - tail;
    if (take > cap) take = cap;
    for (uint32_t i = 0; i < take; ++i) out[i] = b->events[(tail + i) & b->event_mask];
    if (take) b->ev_tail.store(tail + take, std::memory_order_release);
    *n = take;
    return 1;
}

int pl_bridge_send(pl_bridge* b, const pl_cmd* cmd) {
    if (!b || !cmd) return 0;
    const uint32_t head = b->cmd_head.load(std::memory_order_relaxed);
    const uint32_t tail = b->cmd_tail.load(std::memory_order_acquire);
    if (head - tail > b->cmd_mask) {
        b->cmd_lost.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    b->cmds[head & b->cmd_mask] = *cmd;
    b->cmd_head.store(head + 1, std::memory_order_release);
    return 1;
}

int pl_bridge_take_cmd(pl_bridge* b, pl_cmd* out) {
    if (!b || !out) return 0;
    const uint32_t head = b->cmd_head.load(std::memory_order_acquire);
    const uint32_t tail = b->cmd_tail.load(std::memory_order_relaxed);
    if (head == tail) return 0;
    *out = b->cmds[tail & b->cmd_mask];
    b->cmd_tail.store(tail + 1, std::memory_order_release);
    return 1;
}

uint32_t pl_bridge_events_pending(const pl_bridge* b) {
    if (!b) return 0;
    return b->ev_head.load(std::memory_order_acquire) - b->ev_tail.load(std::memory_order_acquire);
}

uint32_t pl_bridge_cmds_pending(const pl_bridge* b) {
    if (!b) return 0;
    return b->cmd_head.load(std::memory_order_acquire) - b->cmd_tail.load(std::memory_order_acquire);
}

uint64_t pl_bridge_events_lost(const pl_bridge* b) {
    return b ? b->ev_lost.load(std::memory_order_relaxed) : 0;
}

uint64_t pl_bridge_cmds_lost(const pl_bridge* b) {
    return b ? b->cmd_lost.load(std::memory_order_relaxed) : 0;
}

int pl_bridge_log(pl_bridge* b, int32_t level, const char* text) {
    if (!b) return 0;
    const uint32_t head = b->log_head.load(std::memory_order_relaxed);
    const uint32_t tail = b->log_tail.load(std::memory_order_acquire);
    if (head - tail > b->log_mask) {
        b->log_lost.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    pl_log& slot = b->logs[head & b->log_mask];
    slot.stamp_us = mono_us();
    slot.seq = head;
    slot.level = level;
    int i = 0;
    if (text) {
        for (; text[i] && i + 1 < PL_LOG_TEXT; ++i) slot.text[i] = text[i];
    }
    slot.text[i] = 0;
    b->log_head.store(head + 1, std::memory_order_release);
    return 1;
}

uint32_t pl_bridge_logs_pending(const pl_bridge* b) {
    if (!b) return 0;
    return b->log_head.load(std::memory_order_acquire) - b->log_tail.load(std::memory_order_acquire);
}

int pl_bridge_take_log(pl_bridge* b, pl_log* out) {
    if (!b || !out) return 0;
    const uint32_t head = b->log_head.load(std::memory_order_acquire);
    const uint32_t tail = b->log_tail.load(std::memory_order_relaxed);
    if (head == tail) return 0;
    *out = b->logs[tail & b->log_mask];
    b->log_tail.store(tail + 1, std::memory_order_release);
    return 1;
}

uint64_t pl_bridge_logs_lost(const pl_bridge* b) {
    return b ? b->log_lost.load(std::memory_order_relaxed) : 0;
}

int64_t pl_now_us(void) { return mono_us(); }

const char* pl_version(void) { return PITLANE_VERSION_STR; }

} // extern "C"
