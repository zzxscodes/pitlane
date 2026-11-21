#pragma once

// Thin C ABI over the cold-path study tools: block tape reads, tape stats, and
// a single-name backtest. Intended for ctypes / FFI callers (see python/).
// The trading library itself stays header-only; this ABI is an optional
// shared library target and is never linked into the hot path.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PL_TAPE_COLUMNS 8

typedef struct pl_tape pl_tape;

typedef struct pl_tick {
    int64_t exch_us;
    int64_t local_us;
    int64_t bid_px;
    int64_t ask_px;
    int64_t last_px;
    int64_t bid_qty;
    int64_t ask_qty;
    int64_t last_qty;
} pl_tick;

typedef struct pl_meta {
    uint32_t versions;
    uint32_t blocks;
    uint32_t symbol;
    uint64_t rows;
    uint64_t bytes;
    int64_t first_ts;
    int64_t last_ts;
} pl_meta;

typedef struct pl_stats {
    uint32_t version;
    uint32_t pad;
    uint64_t rows;
    int64_t min[PL_TAPE_COLUMNS];
    int64_t max[PL_TAPE_COLUMNS];
    int64_t last[PL_TAPE_COLUMNS];
    double mean[PL_TAPE_COLUMNS];
} pl_stats;

typedef struct pl_query {
    uint32_t version;   /* 0 = latest */
    int64_t since_us;   /* inclusive */
    int64_t until_us;   /* inclusive */
    uint32_t offset;
    uint32_t limit;     /* 0 = no limit */
    uint8_t cols;       /* bit i selects column i */
    uint8_t pad[3];
} pl_query;

typedef struct pl_bt_cfg {
    int64_t cash;
    int64_t qty;
    int64_t fee_ppm;
    int64_t min_fee;
    int64_t slip_ticks;
    int64_t tick;
    int64_t threshold_ppm;
    int64_t cooldown_us;
    int8_t sign;
    uint8_t kind;
    uint16_t window;
    int32_t stride;
} pl_bt_cfg;

typedef struct pl_bt_metrics {
    int64_t points;
    int64_t trades;
    int64_t round_trips;
    int64_t wins;
    int64_t gross_pnl;
    int64_t fees;
    int64_t net_pnl;
    int64_t turnover;
    int64_t return_ppm;
    int64_t annual_ppm;
    int64_t sharpe_ppm;
    int64_t max_dd_ppm;
    int64_t win_ppm;
    int64_t exposure_ppm;
    int64_t avg_trade_pnl;
    int64_t span_us;
    int64_t final_equity;
    int64_t peak_equity;
} pl_bt_metrics;

/* ---- Strategy bridge ----------------------------------------------------
 *
 * Two wait-free single-producer/single-consumer rings connect a C++ engine to
 * a Python strategy process or thread: events flow engine -> strategy, commands
 * (orders and queries) flow strategy -> engine. Both records are 64 bytes and
 * plain data; one engine thread posts and takes commands, one strategy thread
 * polls and sends. A full ring never blocks: the record is dropped and counted.
 */

enum pl_event_kind {
    PL_EV_QUOTE = 1,
    PL_EV_ORDER = 2,
    PL_EV_TRADE = 3,
    PL_EV_SIGNAL = 4,
    PL_EV_ASSET = 5,
    PL_EV_POSITION = 6,
    PL_EV_PACKAGE_REQ = 7,
    PL_EV_PACKAGE_RSP = 8,
    PL_EV_TIMER = 9,
    PL_EV_REBUILD_QUOTE = 10,
    PL_EV_MKT_ORDER = 11,
    PL_EV_MKT_TRADE = 12,
    PL_EV_ALGO_REQ = 13,
    PL_EV_ALGO_RSP = 14,
    PL_EV_SIGNAL_BATCH = 15,
    PL_EV_CONTROL_REQ = 16,
    PL_EV_CONTROL_RSP = 17,
    PL_EV_PROPERTY = 18,
    PL_EV_LOG = 19,
    PL_EV_NEW_PARAM = 20,
    PL_EV_ONE_PARAM_DONE = 21,
    PL_EV_NEW_DATE = 22,
    PL_EV_ONE_DATE_DONE = 23,
    PL_EV_BACKTEST_DONE = 24
};

enum pl_cmd_kind {
    PL_CMD_INSERT = 1,
    PL_CMD_CANCEL = 2,
    PL_CMD_PAUSE = 3,
    PL_CMD_RESUME = 4,
    PL_CMD_SUBSCRIBE = 5,
    PL_CMD_UNSUBSCRIBE = 6,
    PL_CMD_QUERY_ASSET = 7,
    PL_CMD_QUERY_POSITION = 8,
    PL_CMD_SIGNAL_SUB = 9,
    PL_CMD_SIGNAL_UNSUB = 10,
    PL_CMD_INSERT_ALGO = 11,
    PL_CMD_CANCEL_ALGO = 12,
    PL_CMD_SUB_MKT_ORDER = 13,
    PL_CMD_SUB_MKT_TRADE = 14,
    PL_CMD_SUB_REBUILD = 15,
    PL_CMD_QUERY_SOURCE = 16,
    PL_CMD_SET_LOG_STATE = 17,
    PL_CMD_CONTROL_RSP = 18
};

typedef struct pl_event {
    uint64_t id;      /* order id, signal key, or package id */
    int64_t stamp_us; /* exchange or local decision time */
    int64_t px;       /* last, bid, ask, or fill price */
    int64_t qty;      /* last, bid, ask, or fill quantity */
    int64_t a;        /* kind-specific: ask px, volume, cash, ... */
    int64_t b;        /* kind-specific: ask qty, turnover, ... */
    uint32_t name;    /* instrument id */
    uint16_t seq;
    uint8_t kind;     /* pl_event_kind */
    uint8_t status;   /* order state or signal ready flag */
    uint8_t side;     /* 0 none, 1 buy/long, 2 sell/short */
    uint8_t pad[7];
} pl_event;

typedef struct pl_cmd {
    uint64_t id;  /* client order id or query token */
    int64_t px;
    int64_t qty;
    int64_t a;    /* kind-specific argument */
    int64_t b;
    uint32_t name; /* instrument id */
    uint16_t seq;
    uint8_t kind;  /* pl_cmd_kind */
    uint8_t side;
    uint8_t status;
    uint8_t hedge; /* 0 speculation, 1 arbitrage, 2 hedge; INSERT only */
    uint8_t pad[14];
} pl_cmd;

#define PL_LOG_TEXT 112

/* One strategy-side log line. The engine drains it from the log ring. */
typedef struct pl_log {
    int64_t stamp_us;
    uint32_t seq;
    int32_t level; /* 0 trace, 1 debug, 2 info, 3 warn, 4 error */
    char text[PL_LOG_TEXT];
} pl_log;

typedef struct pl_bridge pl_bridge;

/* Zero capacities select 4096 events and 1024 commands; both round up to a
 * power of two and are capped at 2^20. The log ring holds 1024 lines.
 * Returns NULL on allocation failure. */
pl_bridge* pl_bridge_open(uint32_t event_cap, uint32_t cmd_cap);
void pl_bridge_close(pl_bridge* b);
/* Engine side: 1 accepted, 0 dropped because the event ring is full. */
int pl_bridge_post(pl_bridge* b, const pl_event* ev);
/* Engine side: 1 if one command was taken, 0 when none is pending. */
int pl_bridge_take_cmd(pl_bridge* b, pl_cmd* out);
/* Strategy side: drains up to `cap` events into `out`; 1 on success. */
int pl_bridge_poll(pl_bridge* b, pl_event* out, uint32_t cap, uint32_t* n);
/* Strategy side: 1 accepted, 0 dropped because the command ring is full. */
int pl_bridge_send(pl_bridge* b, const pl_cmd* cmd);
uint32_t pl_bridge_events_pending(const pl_bridge* b);
uint32_t pl_bridge_cmds_pending(const pl_bridge* b);
uint64_t pl_bridge_events_lost(const pl_bridge* b);
uint64_t pl_bridge_cmds_lost(const pl_bridge* b);
/* Strategy side: one bounded log line, text truncated to PL_LOG_TEXT-1 bytes.
 * 1 accepted, 0 dropped because the log ring is full. */
int pl_bridge_log(pl_bridge* b, int32_t level, const char* text);
uint32_t pl_bridge_logs_pending(const pl_bridge* b);
/* Engine side: 1 if one log line was taken, 0 when none is pending. */
int pl_bridge_take_log(pl_bridge* b, pl_log* out);
uint64_t pl_bridge_logs_lost(const pl_bridge* b);
/* Monotonic clock in microseconds; safe to call from either side. */
int64_t pl_now_us(void);

/* Returns NULL on failure (missing file, bad magic, truncated tail). */
pl_tape* pl_tape_open(const char* path);
void pl_tape_close(pl_tape* t);
/* 1 on success, 0 on failure. */
int pl_tape_meta(const pl_tape* t, pl_meta* out);
int pl_tape_rows(pl_tape* t, const pl_query* q, pl_tick* out, uint32_t cap, uint32_t* n);
int pl_tape_describe(pl_tape* t, uint32_t version, pl_stats* out);
/* Writes `n` ticks as one committed version; 1 on success. */
int pl_tape_write(const char* path, uint32_t symbol, const pl_tick* ticks, uint32_t n);
/* Reads a whole tape and runs the backtester; 1 on success. */
int pl_backtest(const char* path, const pl_bt_cfg* cfg, pl_bt_metrics* out);
/* Library version string, e.g. "2.26.0". */
const char* pl_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
