// Ops console adapter for pitlane-trading: command channel, status board,
// risk guard, host sampling, and the HTTP handler. The trading thread only
// touches this object through on_beat; the HTTP thread only reads the status
// board and writes commands.

#pragma once

#include "full_flow.hpp"
#include "jobs.hpp"
#include "study_conf.hpp"
#include "pitlane/clear/clearing.hpp"
#include "pitlane/clear/statement.hpp"
#include "adapters/market_view.hpp"
#include "pitlane/ops/accounts.hpp"
#include "pitlane/ops/alarm.hpp"
#include "pitlane/ops/auth.hpp"
#include "pitlane/ops/canary.hpp"
#include "pitlane/ops/command.hpp"
#include "pitlane/ops/control.hpp"
#include "pitlane/ops/gateway.hpp"
#include "pitlane/ops/health.hpp"
#include "pitlane/ops/host.hpp"
#include "pitlane/ops/http.hpp"
#include "pitlane/ops/instruments.hpp"
#include "pitlane/ops/logsearch.hpp"
#include "pitlane/ops/policy.hpp"
#include "pitlane/ops/replayctl.hpp"
#include "pitlane/ops/seats.hpp"
#include "pitlane/ops/series.hpp"
#include "pitlane/ops/status.hpp"
#include "pitlane/ops/strategy.hpp"
#include "pitlane/order/credit.hpp"
#include "pitlane/order/ledger_feed.hpp"
#include "pitlane/order/netting.hpp"
#include "pitlane/order/pair_package.hpp"
#include "pitlane/order/unlock.hpp"
#include "pitlane/strat/pair.hpp"
#include "pitlane/study/block_tape.hpp"
#include "pitlane/time/baseline.hpp"
#include "pitlane/time/cycle.hpp"
#include "pitlane/time/flame.hpp"
#include "pitlane/time/hops.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <atomic>
#include <chrono>
#include <thread>

namespace trading_ops {

// Returns the value start after an object key. The quote must follow `{` or `,`
// (whitespace allowed) and close before `:`, so a key name never matches the
// same text inside a string value, such as {"action":"password"}.
inline const char* json_field(const char* s, uint32_t n, const char* key) noexcept {
    if (!s || n == 0 || !key || !*key) return nullptr;
    char pat[40];
    std::snprintf(pat, sizeof pat, "\"%s\"", key);
    const std::size_t plen = std::strlen(pat);
    const char* end = s + n;
    for (const char* p = std::strstr(s, pat); p; p = std::strstr(p + plen, pat)) {
        const char* b = p;
        while (b > s && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) --b;
        if (b != s && b[-1] != '{' && b[-1] != ',') continue;
        const char* q = p + plen;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) ++q;
        if (q < end && *q == ':') return q + 1;
    }
    return nullptr;
}

inline bool key_i64(const char* s, uint32_t n, const char* key, int64_t& out) noexcept {
    const char* p = json_field(s, n, key);
    if (!p) return false;
    const char* end = s + n;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    char* stop = nullptr;
    const long long v = std::strtoll(p, &stop, 10);
    if (stop == p) return false;
    out = v;
    return true;
}

inline bool key_str(const char* s, uint32_t n, const char* key, char* out, int cap) noexcept {
    if (!out || cap <= 0) return false;
    const char* p = json_field(s, n, key);
    if (!p) return false;
    const char* end = s + n;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end || *p != '"') return false;
    ++p;
    int i = 0;
    while (p < end && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return p < end;
}

// Parses `key=123` from a query string (no quotes, unlike JSON).
inline bool query_i64(const char* q, const char* key, int64_t& out) noexcept {
    if (!q || !key || !*key) return false;
    const std::size_t klen = std::strlen(key);
    const char* p = q;
    while ((p = std::strstr(p, key)) != nullptr) {
        const bool boundary = (p == q) || p[-1] == '&' || p[-1] == '?';
        if (boundary && p[klen] == '=') {
            char* stop = nullptr;
            const long long v = std::strtoll(p + klen + 1, &stop, 10);
            if (stop != p + klen + 1) {
                out = v;
                return true;
            }
        }
        p += klen;
    }
    return false;
}

// Decodes `%XX` in place; `+` is left alone because it is a literal here.
inline void url_decode(char* s) noexcept {
    char* w = s;
    for (const char* r = s; *r; ++r) {
        if (*r == '%' && r[1] && r[2]) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(r[1]);
            const int lo = hex(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = static_cast<char>(hi * 16 + lo);
                r += 2;
                continue;
            }
        }
        *w++ = *r;
    }
    *w = 0;
}

// Copies a `key=value` query parameter into `out` and percent-decodes it.
inline bool query_str(const char* q, const char* key, char* out, int cap) noexcept {
    if (!q || !key || !*key || !out || cap <= 0) return false;
    const std::size_t klen = std::strlen(key);
    const char* p = q;
    while ((p = std::strstr(p, key)) != nullptr) {
        const bool boundary = (p == q) || p[-1] == '&' || p[-1] == '?';
        if (boundary && p[klen] == '=') {
            p += klen + 1;
            int i = 0;
            while (*p && *p != '&' && i + 1 < cap) out[i++] = *p++;
            out[i] = 0;
            url_decode(out);
            return true;
        }
        p += klen;
    }
    return false;
}

inline int key_legs(const char* s, uint32_t n, pitlane::OpsLeg* legs, int max) noexcept {
    const char* p = n ? std::strstr(s, "\"legs\"") : nullptr;
    if (!p) return -1;
    p = std::strchr(p, '[');
    if (!p) return -1;
    ++p;
    int count = 0;
    while (*p && *p != ']') {
        const char* obj = std::strchr(p, '{');
        if (!obj) break;
        const char* close = std::strchr(obj, '}');
        if (!close) return -1;
        char tmp[256];
        const std::size_t len = static_cast<std::size_t>(close - obj) + 1;
        if (len >= sizeof tmp) return -1;
        std::memcpy(tmp, obj, len);
        tmp[len] = 0;
        int64_t name = 0;
        int64_t target = 0;
        if (!key_i64(tmp, static_cast<uint32_t>(len), "name", name)) return -1;
        if (!key_i64(tmp, static_cast<uint32_t>(len), "target", target)) return -1;
        if (count < max) {
            legs[count].name = static_cast<uint32_t>(name);
            legs[count].target = target;
        }
        ++count;
        p = close + 1;
    }
    return count > max ? -1 : count;
}

// Parses `rows` for basket netting: `acct`, `counter`, `name`, `side`, `qty`.
// Account and counter are optional and stay zero when absent.
inline constexpr int kNetRows = 32;

inline int key_net_rows(const char* s, uint32_t n, pitlane::NetOrder* rows, int max) noexcept {
    const char* p = json_field(s, n, "rows");
    if (!p) return -1;
    p = std::strchr(p, '[');
    if (!p) return -1;
    ++p;
    int count = 0;
    while (*p && *p != ']') {
        const char* obj = std::strchr(p, '{');
        if (!obj) break;
        const char* close = std::strchr(obj, '}');
        if (!close) return -1;
        char tmp[256];
        const std::size_t len = static_cast<std::size_t>(close - obj) + 1;
        if (len >= sizeof tmp) return -1;
        std::memcpy(tmp, obj, len);
        tmp[len] = 0;
        int64_t acct = 0;
        int64_t counter = 0;
        int64_t name = 0;
        int64_t side = 0;
        int64_t qty = 0;
        if (!key_i64(tmp, static_cast<uint32_t>(len), "name", name)) return -1;
        if (!key_i64(tmp, static_cast<uint32_t>(len), "side", side)) return -1;
        if (!key_i64(tmp, static_cast<uint32_t>(len), "qty", qty)) return -1;
        (void)key_i64(tmp, static_cast<uint32_t>(len), "acct", acct);
        (void)key_i64(tmp, static_cast<uint32_t>(len), "counter", counter);
        if (count < max) {
            rows[count].acct = static_cast<uint32_t>(acct);
            rows[count].counter = static_cast<uint32_t>(counter);
            rows[count].name = static_cast<uint32_t>(name);
            rows[count].side = static_cast<uint8_t>(side);
            rows[count].qty = qty;
        }
        ++count;
        p = close + 1;
    }
    return count > max ? -1 : count;
}

// Reads `key = value` pairs. Returns false only when the file cannot be opened.
// `bind` may be null when the caller does not want to reload the listen address.
inline bool load_ops_conf(const char* path, pitlane::OpsPolicy& pol, uint16_t& port, char* root, int cap,
                          char* bind = nullptr, int bind_cap = 0) noexcept {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == 0 || *p == '\n' || *p == '\r' || *p == '#' || *p == '[') continue;
        char* eq = std::strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = p;
        char* val = eq + 1;
        char* ke = key + std::strlen(key);
        while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        while (*val == ' ' || *val == '\t') ++val;
        char* ve = val + std::strlen(val);
        while (ve > val && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
        const long long v = std::strtoll(val, nullptr, 10);
        if (std::strcmp(key, "port") == 0) port = static_cast<uint16_t>(v);
        else if (std::strcmp(key, "doc_root") == 0) std::snprintf(root, static_cast<std::size_t>(cap), "%s", val);
        else if (std::strcmp(key, "bind") == 0 && bind && bind_cap > 0)
            std::snprintf(bind, static_cast<std::size_t>(bind_cap), "%s", val);
        else if (std::strcmp(key, "max_manual_qty") == 0) pol.max_manual_qty = v;
        else if (std::strcmp(key, "max_manual_notional") == 0) pol.max_manual_notional = v;
        else if (std::strcmp(key, "max_manual_per_min") == 0) pol.max_manual_per_min = static_cast<int32_t>(v);
        else if (std::strcmp(key, "max_daily_loss") == 0) pol.max_daily_loss = v;
        else if (std::strcmp(key, "max_drawdown_ppm") == 0) pol.max_drawdown_ppm = v;
        else if (std::strcmp(key, "max_reject_ppm") == 0) pol.max_reject_ppm = static_cast<int32_t>(v);
        else if (std::strcmp(key, "stale_tick_us") == 0) pol.stale_tick_us = v;
        else if (std::strcmp(key, "max_jump_ppm") == 0) pol.max_jump_ppm = v;
    }
    std::fclose(f);
    return true;
}

inline void parse_names(const char* v, pitlane::NameSet& set) noexcept {
    if (!v) return;
    const char* p = v;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        char* stop = nullptr;
        const unsigned long x = std::strtoul(p, &stop, 10);
        if (stop == p) {
            ++p;
            continue;
        }
        if (x > 0 && x <= 0xfffffffful) (void)set.add(static_cast<uint32_t>(x));
        p = stop;
    }
}

inline void parse_i64_row(const char* v, int64_t* out, int cap) noexcept {
    if (!v) return;
    int i = 0;
    const char* p = v;
    while (*p && i < cap) {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        char* stop = nullptr;
        out[i++] = std::strtoll(p, &stop, 10);
        if (stop == p) break;
        p = stop;
    }
}

// Pre-trade rules share the ops config file. Every key is optional and a zero
// limit disables that rule, so an unconfigured install only records counters.
inline void load_pre_conf(const char* path, pitlane::PreLimits& lim, pitlane::NameSet& open,
                          pitlane::NameSet& close) noexcept {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == 0 || *p == '\n' || *p == '\r' || *p == '#' || *p == '[') continue;
        char* eq = std::strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = p;
        char* val = eq + 1;
        char* ke = key + std::strlen(key);
        while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        while (*val == ' ' || *val == '\t') ++val;
        char* ve = val + std::strlen(val);
        while (ve > val && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
        if (std::strcmp(key, "pre_max_notional") == 0) parse_i64_row(val, lim.max_notional, 8);
        else if (std::strcmp(key, "pre_max_orders_1s") == 0) lim.max_orders_1s = static_cast<int32_t>(std::strtoll(val, nullptr, 10));
        else if (std::strcmp(key, "pre_max_orders_10s") == 0) lim.max_orders_10s = static_cast<int32_t>(std::strtoll(val, nullptr, 10));
        else if (std::strcmp(key, "pre_max_market_orders_1s") == 0) lim.max_market_orders_1s = static_cast<int32_t>(std::strtoll(val, nullptr, 10));
        else if (std::strcmp(key, "pre_max_cancel_ratio_ppm") == 0) lim.max_cancel_ratio_ppm = static_cast<int32_t>(std::strtoll(val, nullptr, 10));
        else if (std::strcmp(key, "pre_min_orders_for_ratio") == 0) lim.min_orders_for_ratio = static_cast<int32_t>(std::strtoll(val, nullptr, 10));
        else if (std::strcmp(key, "pre_max_cancel_amount_ratio_ppm") == 0) lim.max_cancel_amount_ratio_ppm = static_cast<int32_t>(std::strtoll(val, nullptr, 10));
        else if (std::strcmp(key, "pre_cancel_amount_window_s") == 0) lim.cancel_amount_window_s = static_cast<int32_t>(std::strtoll(val, nullptr, 10));
        else if (std::strcmp(key, "pre_self_trade") == 0) lim.self_trade_block = std::strtoll(val, nullptr, 10) != 0 ? 1 : 0;
        else if (std::strcmp(key, "pre_open_blacklist") == 0) parse_names(val, open);
        else if (std::strcmp(key, "pre_close_blacklist") == 0) parse_names(val, close);
    }
    std::fclose(f);
}

// Resolves a sibling of the ops config, e.g. run/ops.conf + users.conf.
inline void conf_sibling(const char* conf, const char* name, char* out, int cap) noexcept {
    const char* slash = conf ? std::strrchr(conf, '/') : nullptr;
    if (!slash) {
        std::snprintf(out, static_cast<std::size_t>(cap), "%s", name);
        return;
    }
    const int dir = static_cast<int>(slash - conf) + 1;
    if (dir >= cap) {
        std::snprintf(out, static_cast<std::size_t>(cap), "%s", name);
        return;
    }
    std::memcpy(out, conf, static_cast<std::size_t>(dir));
    out[dir] = 0;
    std::snprintf(out + dir, static_cast<std::size_t>(cap - dir), "%s", name);
}

struct GwHooks {
    int (*start)(void* user, const char* id){nullptr};
    int (*stop)(void* user, const char* id){nullptr};
    // Optional desk timeout counters. The console shows them next to the
    // reject-rate; a null hook leaves the fields at zero.
    void (*timeouts)(void* user, int64_t* ack, int64_t* life, int64_t* pull){nullptr};
    void* user{nullptr};
};

// Trading-thread runtime for one strategy instance. The factor engine is fed
// only from newly observed ticks; the cursor avoids replaying the ring.
struct StratRt {
    pitlane::FactorEngine<1, 256> eng;
    pitlane::PairEngine pair;
    int cursor{0};
    int cursor_b{0};
    int64_t next_emit_us[2]{};
    int64_t last_sig_us{0};
    int64_t signals{0};
    int64_t orders{0};
    int64_t rejects{0};
    int64_t last_factor{0};
    int64_t position{0};
    int64_t started_us{0};
};

class SimOps {
public:
    ~SimOps() { close(); }

    bool open(pitlane::FullFlow& flow, bool paper, uint16_t port, const char* doc_root, const char* ops_conf,
              const char* bind_addr = nullptr, const char* tape_path = nullptr, const GwHooks* hooks = nullptr) noexcept {
        flow_ = &flow;
        paper_ = paper;
        pol_ = pitlane::OpsPolicy{};
        const bool root_explicit = doc_root && doc_root[0];
        std::snprintf(root_, sizeof root_, "%s", root_explicit ? doc_root : "");
        std::snprintf(bind_, sizeof bind_, "127.0.0.1");
        std::snprintf(conf_path_, sizeof conf_path_, "%s", ops_conf ? ops_conf : "run/ops.conf");
        std::snprintf(tape_path_, sizeof tape_path_, "%s", tape_path ? tape_path : "");
        uint16_t conf_port = 0;
        char conf_root[256]{};
        const bool have_conf =
            trading_ops::load_ops_conf(conf_path_, pol_, conf_port, conf_root, sizeof conf_root, bind_, sizeof bind_);
        // An explicit --web-root or built-in client directory wins over the
        // conf file; the conf value only fills the gap.
        if (!root_explicit) std::snprintf(root_, sizeof root_, "%s", conf_root[0] ? conf_root : "web");
        if (port == 0) port = conf_port;
        if (bind_addr && bind_addr[0]) std::snprintf(bind_, sizeof bind_, "%s", bind_addr);
        if (!have_conf) {
            std::printf("[ops] %s not found, using defaults\n", conf_path_);
        }
        pre_.reset();
        trading_ops::load_pre_conf(conf_path_, pre_limits_, pre_.open_blacklist(), pre_.close_blacklist());
        pre_.set_limits(pre_limits_);
        flow.set_precheck(&pre_);
        if (hooks) hooks_ = *hooks;

        json_ = static_cast<char*>(std::malloc(kJsonCap));
        if (!json_) return false;

        trading_ops::conf_sibling(conf_path_, "users.conf", users_path_, sizeof users_path_);
        trading_ops::conf_sibling(conf_path_, "gateways.conf", gw_path_, sizeof gw_path_);
        trading_ops::conf_sibling(conf_path_, "accounts.conf", acc_path_, sizeof acc_path_);
        trading_ops::conf_sibling(conf_path_, "instruments.conf", instr_path_, sizeof instr_path_);
        if (!accs_.load(acc_path_)) {
            std::printf("[ops] %s not found, using one default account\n", acc_path_);
            pitlane::AccountSpec a{};
            std::snprintf(a.id, sizeof a.id, "default");
            std::snprintf(a.name, sizeof a.name, "Default");
            std::snprintf(a.klass, sizeof a.klass, "astock");
            std::snprintf(a.ccy, sizeof a.ccy, "CNY");
            a.cash = 1000000000;
            a.env = pitlane::kEnvTest;
            a.num = 1;
            accs_.inject(a);
        }
        if (!instr_.load(instr_path_)) {
            std::printf("[ops] %s not found, instruments use defaults\n", instr_path_);
        }
        // The risk KPI starts from the account's own cash, so a live account and
        // the equity card agree. Fall back to the built-in base when none matches.
        equity_base_ = kEquityBase;
        const uint8_t want_env = paper_ ? pitlane::kEnvTest : pitlane::kEnvLive;
        for (int i = 0; i < accs_.count(); ++i) {
            const pitlane::AccountSpec& a = accs_.spec(i);
            if (a.env == want_env && a.cash > 0) {
                equity_base_ = a.cash;
                break;
            }
        }
        for (int i = 0; i < accs_.count() && i < 8; ++i) {
            books_[i].reset(accs_.spec(i).num, accs_.spec(i).env);
            // Accounts hold cash in 1e-6 units; the clearing book works in whole
            // currency units, like its fees, margin, and market value.
            books_[i].set_cash(accs_.spec(i).cash / 1000000);
            // The account currency is the base; other product currencies convert
            // through the instrument's fx_ppm. Shares start at the cash balance,
            // so the unit NAV opens at 1.0.
            books_[i].set_base_ccy(accs_.spec(i).ccy);
            books_[i].set_shares(accs_.spec(i).cash / 1000000);
        }
        if (!users_.load(users_path_)) {
            if (users_.add("admin", "pitlane", pitlane::kRoleAdmin, true, users_path_)) {
                std::printf("[ops] created %s with admin/pitlane; change it in the console\n", users_path_);
            } else {
                std::printf("[ops] no accounts in %s; the console cannot log in\n", users_path_);
            }
        }
        load_gateways();
        trading_ops::conf_sibling(conf_path_, "strategies.conf", strat_path_, sizeof strat_path_);
        load_strategies();
        trading_ops::conf_sibling(conf_path_, "packages.conf", pkg_path_, sizeof pkg_path_);
        trading_ops::conf_sibling(conf_path_, "credit.conf", credit_path_, sizeof credit_path_);
        load_packages();
        load_credit();
        seed_seats();
        trading_ops::conf_sibling(conf_path_, "perf-baseline.txt", base_path_, sizeof base_path_);
        load_baseline();
        scale_ok_ = pitlane::cycle_warm(scale_, 10);
        logs_.add(now_us(), pitlane::kLogInfo, pitlane::kLogOps, "console open");

        if (tape_path && tape_path[0]) {
            const int fed = pitlane::series_seed_tape(series_, tape_path);
            std::printf("[ops] seeded %d ticks from %s\n", fed, tape_path);
        }
        flow.set_series(&series_);
        flow.set_alarms(&alarms_);
        flow.set_clearing(&SimOps::clearing_cb, this);
        flow.set_short_ok(&SimOps::short_ok_cb, this);

        cp_.open(&ch_, &flow.rms(), pol_);
        cp_.set_account(1);
        cp_.set_mint(&flow.mint());
        cp_.set_send(&SimOps::send_cb, this);
        cp_.set_top(&SimOps::top_cb, this);
        cp_.set_net(&SimOps::net_cb, this);
        cp_.set_reload(&SimOps::reload_cb, this);
        cp_.set_cancel(&SimOps::cancel_cb, this);
        cp_.set_gateway(&SimOps::gw_start_cb, &SimOps::gw_stop_cb, this);
        cp_.set_alarms(&alarms_, pitlane::kEnvTest);
        cp_.set_reason(&SimOps::reason_cb, this);
        flow.set_control(&cp_);
        flow.set_ops(&SimOps::beat_cb, this);
        guard_.set(pol_);
        start_us_ = now_us();
        sampler_stop_.store(false, std::memory_order_relaxed);
        sample_seq_.store(0, std::memory_order_relaxed);
        sample_seen_ = 0;
        last_risk_us_ = 0;
        last_snap_us_ = 0;
        peak_equity_ = equity_base_;
        flow_ = &flow;

        start_sampler();
        if (port == 0) {
            std::printf("[ops] web disabled\n");
            return true;
        }
        if (!jobs_.start()) {
            std::printf("[ops] job worker failed to start; research and backtest disabled\n");
        }
        if (!http_.start(port, &SimOps::http_cb, this, root_, bind_)) {
            std::printf("[ops] HTTP bind on %s:%u failed; trading continues without web\n", bind_,
                        static_cast<unsigned>(port));
        }
        return true;
    }

    void close() noexcept {
        stop_sampler();
        jobs_.stop();
        http_.stop();
        if (json_) {
            std::free(json_);
            json_ = nullptr;
        }
        flow_ = nullptr;
    }

    bool web() const noexcept { return http_.running(); }
    uint16_t port() const noexcept { return http_.port(); }
    const char* bind_addr() const noexcept { return bind_; }

    // The live client logs in directly instead of through a hub-spawned process.
    // Reflect that connection in the registry so the console shows it running.
    void note_direct_gateway(bool on) noexcept {
        const uint8_t env = paper_ ? pitlane::kEnvTest : pitlane::kEnvLive;
        const int64_t now = now_us();
        for (int i = 0; i < gw_.count(); ++i) {
            const pitlane::GwSpec& s = gw_.spec(i);
            if (s.kind != pitlane::kGwTd || s.env_bit != env) continue;
            gw_.mark(i, on ? pitlane::kGwRunning : pitlane::kGwStopped, on ? "direct" : "stopped", now);
            const uint32_t sid = static_cast<uint32_t>(i + 1);
            if (on) (void)seats_.start(sid, now);
            else (void)seats_.stop(sid);
            return;
        }
    }

    pitlane::ControlPlane& control() noexcept { return cp_; }
    const pitlane::OpsPolicy& policy() const noexcept { return pol_; }

    // 绑定行情端的实时视图与逐跳账本。两个指针都由调用方持有，生命周期覆盖本对象。
    void set_market(const pitlane::MarketBoard* live, const pitlane::HopLedger* hops) noexcept {
        market_live_ = live;
        md_hops_ = hops;
    }

    // 柜台账本。CTP 回调线程写入整份快照，HTTP 线程读取；未绑定轮询时（paper）
    // 由清结算报告每秒填充一份模拟柜台视图。
    using LedgerPollFn = void (*)(void* user);
    void set_ledger_poll(LedgerPollFn fn, void* user) noexcept {
        ledger_poll_ = fn;
        ledger_user_ = user;
    }
    static void ledger_cb(void* user, int kind, const pitlane::LedgerAsset* assets, int n_assets,
                          const pitlane::LedgerPosition* positions, int n_positions, int64_t stamp_us) noexcept {
        static_cast<SimOps*>(user)->ingest_ledger(kind, assets, n_assets, positions, n_positions, stamp_us);
    }
    const pitlane::LedgerFeed& ledger() const noexcept { return ledger_; }
    // 合约乘数由 instruments.conf 提供，柜台账本用它把成本的总额换算成均价。
    int64_t instrument_mult(uint32_t name) const noexcept {
        const pitlane::InstrSpec* s = instr_.by_name(name);
        return s ? s->mult : 0;
    }

private:
    static constexpr int64_t kEquityBase = 1000000000;
    static constexpr int kJsonCap = 65536;

    static int64_t now_us() noexcept {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000000ll + ts.tv_nsec / 1000;
    }

    static int send_cb(void* user, const pitlane::Intent& in) noexcept {
        auto* self = static_cast<SimOps*>(user);
        if (!self->flow_) return 2;
        return self->flow_->submit_manual(in, in.stamp_us > 0 ? in.stamp_us : self->now_us());
    }

    static bool short_ok_cb(void* user, uint32_t name) noexcept {
        auto* self = static_cast<SimOps*>(user);
        const pitlane::InstrSpec* s = self->instr_.by_name(name);
        return s ? s->allow_short : true;
    }

    static const char* reason_cb(void* user) noexcept {
        auto* self = static_cast<SimOps*>(user);
        return self->flow_ ? self->flow_->last_reject_text() : "";
    }

    // Best bid/ask for any product: the tape the console already serves, with
    // the strategy's name-1 quote as a fallback. This is what makes px=0 mean
    // "take the opposite side" for every product, not just the strategy's.
    static void top_cb(void* user, uint32_t name, pitlane::ManualQuote& out) noexcept {
        auto* self = static_cast<SimOps*>(user);
        pitlane::SeriesRing<2048>* ring = self->series_.find(name);
        if (ring && ring->size() > 0) {
            const pitlane::SeriesTick t = ring->at(ring->size() - 1);
            out.bid = t.bid;
            out.ask = t.ask;
        }
        if (out.bid <= 0 || out.ask <= 0) {
            int64_t bid = 0;
            int64_t ask = 0;
            if (self->flow_) self->flow_->last_quote(name, bid, ask);
            if (out.bid <= 0) out.bid = bid;
            if (out.ask <= 0) out.ask = ask;
        }
    }

    static int64_t net_cb(void* user, uint32_t name) noexcept {
        auto* self = static_cast<SimOps*>(user);
        return self->flow_ ? self->flow_->net_qty(name) : 0;
    }

    static bool reload_cb(void* user, pitlane::OpsPolicy& out) noexcept {
        auto* self = static_cast<SimOps*>(user);
        pitlane::OpsPolicy next = self->pol_;
        uint16_t port = 0;
        char root[256];
        std::snprintf(root, sizeof root, "%s", self->root_);
        if (!trading_ops::load_ops_conf(self->conf_path_, next, port, root, sizeof root)) return false;
        self->pol_ = next;
        self->guard_.set(next);
        out = next;
        return true;
    }

    static void cancel_cb(void* user) noexcept {
        auto* self = static_cast<SimOps*>(user);
        if (self->flow_) self->flow_->cancel_all();
    }

    static int gw_start_cb(void* user, const char* id) noexcept {
        auto* self = static_cast<SimOps*>(user);
        return self->start_gateway(self->gw_.index_of(id), self->now_us());
    }

    static int gw_stop_cb(void* user, const char* id) noexcept {
        auto* self = static_cast<SimOps*>(user);
        return self->stop_gateway(self->gw_.index_of(id), self->now_us());
    }

    // ---- manual pair packages -------------------------------------------
    // Desk bookkeeping the operator creates, adjusts, and cancels by hand.
    // Persisted next to the other ops files so a restart keeps the book; the
    // file is only touched by the HTTP thread.
    void load_packages() noexcept {
        packages_.reset();
        std::FILE* f = std::fopen(pkg_path_, "r");
        if (!f) return;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (std::strncmp(p, "pkg", 3) != 0) continue;
            unsigned id = 0;
            unsigned st = 0;
            unsigned a = 0;
            unsigned b = 0;
            long long ratio = 0;
            long long qa = 0;
            long long qb = 0;
            long long fa = 0;
            long long fb = 0;
            long long fx = 0;
            char note[40]{};
            if (std::sscanf(p, "pkg = %u %u %u %u %lld %lld %lld %lld %lld %lld %39s", &id, &st, &a, &b, &ratio, &qa,
                            &qb, &fa, &fb, &fx, note) != 11) {
                continue;
            }
            pitlane::PairPackage row{};
            row.id = id;
            row.state = static_cast<uint8_t>(st);
            row.name_a = a;
            row.name_b = b;
            row.ratio_ppm = ratio;
            row.qty_a = qa;
            row.qty_b = qb;
            row.filled_a = fa;
            row.filled_b = fb;
            row.fx_ppm = fx;
            if (std::strcmp(note, "-") != 0) pitlane::pair_package_note(row.note, sizeof row.note, note);
            (void)packages_.restore(row);
        }
        std::fclose(f);
    }

    void save_packages() noexcept {
        if (!pkg_path_[0]) return;
        std::FILE* f = std::fopen(pkg_path_, "w");
        if (!f) return;
        std::fprintf(f, "# pair packages: id state name_a name_b ratio_ppm qty_a qty_b filled_a filled_b fx_ppm note\n");
        pitlane::PairPackage rows[kPkgCap]{};
        const int n = packages_.list(rows, kPkgCap);
        for (int i = 0; i < n; ++i) {
            const pitlane::PairPackage& r = rows[i];
            std::fprintf(f, "pkg = %u %u %u %u %lld %lld %lld %lld %lld %lld %s\n", r.id,
                         static_cast<unsigned>(r.state), r.name_a, r.name_b, static_cast<long long>(r.ratio_ppm),
                         static_cast<long long>(r.qty_a), static_cast<long long>(r.qty_b),
                         static_cast<long long>(r.filled_a), static_cast<long long>(r.filled_b),
                         static_cast<long long>(r.fx_ppm), r.note[0] ? r.note : "-");
        }
        std::fclose(f);
    }

    // ---- credit terms ----------------------------------------------------
    // A credit adapter fills these from the counter asset query; until one is
    // wired an operator can enter the same fields by hand.
    void load_credit() noexcept {
        credit_.reset();
        std::FILE* f = std::fopen(credit_path_, "r");
        if (!f) return;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (std::strncmp(p, "term", 4) == 0) {
                unsigned acct = 0;
                unsigned src = 0;
                long long ab = 0;
                long long td = 0;
                long long fd = 0;
                long long sd = 0;
                long long ar = 0;
                long long fq = 0;
                long long sq = 0;
                if (std::sscanf(p, "term = %u %u %lld %lld %lld %lld %lld %lld %lld", &acct, &src, &ab, &td, &fd, &sd,
                                &ar, &fq, &sq) != 9) {
                    continue;
                }
                pitlane::CreditTerms t{};
                t.asset_balance = ab;
                t.total_debt = td;
                t.fund_debt = fd;
                t.stock_debt = sd;
                t.assure_ratio_ppm = ar;
                t.finance_quota = fq;
                t.shortsell_quota = sq;
                t.source = static_cast<uint8_t>(src);
                (void)credit_.set(acct, t);
            } else if (std::strncmp(p, "cash", 4) == 0) {
                unsigned acct = 0;
                long long lq = 0;
                long long sq = 0;
                long long sp = 0;
                if (std::sscanf(p, "cash = %u %lld %lld %lld", &acct, &lq, &sq, &sp) != 4) continue;
                pitlane::CreditCash c{};
                c.long_qty = lq;
                c.short_qty = sq;
                c.special_short = sp;
                (void)credit_.set_cash(acct, c);
            }
        }
        std::fclose(f);
    }

    void save_credit() noexcept {
        if (!credit_path_[0]) return;
        std::FILE* f = std::fopen(credit_path_, "w");
        if (!f) return;
        std::fprintf(f, "# term: acct source asset_balance total_debt fund_debt stock_debt assure_ppm finance_quota "
                        "shortsell_quota\n");
        std::fprintf(f, "# cash: acct long_qty short_qty special_short\n");
        uint32_t accts[8]{};
        pitlane::CreditTerms terms[8]{};
        pitlane::CreditCash cash[8]{};
        const int n = credit_.list(accts, terms, cash, 8);
        for (int i = 0; i < n; ++i) {
            const pitlane::CreditTerms& t = terms[i];
            std::fprintf(f, "term = %u %u %lld %lld %lld %lld %lld %lld %lld\n", accts[i],
                         static_cast<unsigned>(t.source), static_cast<long long>(t.asset_balance),
                         static_cast<long long>(t.total_debt), static_cast<long long>(t.fund_debt),
                         static_cast<long long>(t.stock_debt), static_cast<long long>(t.assure_ratio_ppm),
                         static_cast<long long>(t.finance_quota), static_cast<long long>(t.shortsell_quota));
        }
        for (int i = 0; i < n; ++i) {
            const pitlane::CreditCash& c = cash[i];
            std::fprintf(f, "cash = %u %lld %lld %lld\n", accts[i], static_cast<long long>(c.long_qty),
                         static_cast<long long>(c.short_qty), static_cast<long long>(c.special_short));
        }
        std::fclose(f);
    }

    void load_gateways() noexcept {
        if (!gw_.load(gw_path_)) {
            std::printf("[ops] %s not found, no gateways configured\n", gw_path_);
            return;
        }
        for (int i = 0; i < gw_.count(); ++i) {
            const pitlane::GwSpec& spec = gw_.spec(i);
            gw_.set_account_num(i, accs_.resolve(spec.acct));
            const bool tape = std::strcmp(spec.adapter, "tape") == 0;
            const bool paper = std::strcmp(spec.adapter, "paper") == 0;
            const bool ctp = std::strcmp(spec.adapter, "ctp") == 0;
            const bool available = spec.env_bit == pitlane::kEnvLive ? (ctp && hooks_.start != nullptr)
                                                                    : (tape || paper || ctp);
            gw_.set_available(i, available);
            if (!available) {
                gw_.mark(i, pitlane::kGwStopped, "adapter unavailable", now_us());
            } else if (spec.auto_start && spec.env_bit == (paper_ ? pitlane::kEnvTest : pitlane::kEnvLive)) {
                (void)start_gateway(i, now_us());
            } else {
                gw_.mark(i, pitlane::kGwStopped, spec.auto_start ? "other mode" : "stopped", now_us());
            }
        }
    }

    int start_gateway(int i, int64_t now) noexcept {
        if (i < 0 || i >= gw_.count()) return 1;
        if (!gw_.spec(i).available) {
            gw_.mark(i, pitlane::kGwFailed, "adapter unavailable", now);
            return 1;
        }
        if (gw_.status(i).state == pitlane::kGwRunning) return 0;
        gw_.mark(i, pitlane::kGwStarting, "starting", now);
        int rc = 0;
        if (paper_) {
            gw_on_[i] = true;
        } else if (hooks_.start) {
            rc = hooks_.start(hooks_.user, gw_.spec(i).id);
        } else {
            rc = 1;
        }
        gw_.mark(i, rc == 0 ? pitlane::kGwRunning : pitlane::kGwFailed, rc == 0 ? "running" : "start failed", now);
        std::printf("[ops] gateway %s %s\n", gw_.spec(i).id, rc == 0 ? "started" : "start failed");
        (void)alarms_.push(pitlane::kAlarmGateway, rc == 0 ? pitlane::kAlarmInfo : pitlane::kAlarmWarn,
                           gw_.spec(i).env_bit, now, "gateway %s %s", gw_.spec(i).id,
                           rc == 0 ? "started" : "start failed");
        return rc;
    }

    int stop_gateway(int i, int64_t now) noexcept {
        if (i < 0 || i >= gw_.count()) return 1;
        if (gw_.status(i).state == pitlane::kGwStopped) return 0;
        int rc = 0;
        if (paper_) {
            gw_on_[i] = false;
        } else if (hooks_.stop) {
            rc = hooks_.stop(hooks_.user, gw_.spec(i).id);
        } else {
            rc = 1;
        }
        gw_.mark(i, rc == 0 ? pitlane::kGwStopped : pitlane::kGwFailed, rc == 0 ? "stopped" : "stop failed", now);
        std::printf("[ops] gateway %s %s\n", gw_.spec(i).id, rc == 0 ? "stopped" : "stop failed");
        (void)alarms_.push(pitlane::kAlarmGateway, rc == 0 ? pitlane::kAlarmInfo : pitlane::kAlarmWarn,
                           gw_.spec(i).env_bit, now, "gateway %s %s", gw_.spec(i).id,
                           rc == 0 ? "stopped" : "stop failed");
        return rc;
    }

    // Stops the trading gateway that serves `name`, or every running td gateway
    // when the instrument does not name one.
    void stop_td_for_name(uint32_t name, int64_t now) noexcept {
        const pitlane::InstrSpec* ins = instr_.by_name(name);
        if (ins && ins->gw[0]) {
            const int g = gw_.index_of(ins->gw);
            if (g >= 0 && gw_.spec(g).kind == pitlane::kGwTd && gw_.status(g).state == pitlane::kGwRunning) {
                (void)stop_gateway(g, now);
                return;
            }
        }
        for (int i = 0; i < gw_.count(); ++i) {
            if (gw_.spec(i).kind == pitlane::kGwTd && gw_.status(i).state == pitlane::kGwRunning) {
                (void)stop_gateway(i, now);
            }
        }
    }

    void audit_risk(uint8_t code, int64_t now) noexcept {
        pitlane::OpsAuditRec rec{};
        rec.seq = ch_.alloc_seq();
        rec.code = code;
        rec.kind = 0xfe;
        rec.stamp_us = now;
        ch_.audit(rec);
    }

    static int beat_cb(void* user, int64_t now_us, pitlane::FullFlow& flow) noexcept {
        return static_cast<SimOps*>(user)->on_beat(flow, now_us);
    }

    // TSC 未标定时所有逐跳统计直接跳过，不影响交易链路。
    uint64_t ns_of(uint64_t begin, uint64_t end) const noexcept {
        uint64_t ns = 0;
        (void)pitlane::scale_span(scale_, begin, end, ns);
        return ns;
    }
    uint64_t ns_abs(uint64_t c) const noexcept { return ns_of(0, c); }

    // 把新增告警同步进日志索引，便于统一检索。
    void log_alarms() noexcept {
        pitlane::Alarm a[8]{};
        const int n = alarms_.recent(a, 8, log_alarm_seq_, 0xff, false);
        for (int i = 0; i < n; ++i) {
            logs_.add(a[i].stamp_us, a[i].sev >= pitlane::kAlarmWarn ? pitlane::kLogWarn : pitlane::kLogInfo,
                      pitlane::kLogRisk, a[i].text);
            log_alarm_seq_ = a[i].seq;
        }
    }

    // 主机采样要读 /proc 与 statfs，放到独立线程每秒做一次；交易线程只取最新一份。
    // 序号为偶数是稳定快照，与 MarketBoard 的 seqlock 用法一致。
    struct SamplerPost {
        pitlane::HostSample host{};
        pitlane::HealthSample health{};
        bool health_ok{false};
    };

    void start_sampler() noexcept {
        if (sampler_.joinable()) return;
        try {
            sampler_ = std::thread(&SimOps::sample_loop, this);
        } catch (...) {
            std::printf("[ops] host sampler thread failed to start; health telemetry disabled\n");
        }
    }

    void stop_sampler() noexcept {
        sampler_stop_.store(true, std::memory_order_relaxed);
        if (sampler_.joinable()) sampler_.join();
    }

    void sample_loop() noexcept {
        while (!sampler_stop_.load(std::memory_order_relaxed)) {
            const int64_t now = SimOps::now_us();
            SamplerPost p{};
            (void)pitlane::host_sample(p.host);
            p.health_ok = pitlane::health_sample(p.health, now);
            const uint64_t v = sample_seq_.load(std::memory_order_relaxed);
            sample_seq_.store(v + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            sample_post_ = p;
            std::atomic_thread_fence(std::memory_order_release);
            sample_seq_.store(v + 2, std::memory_order_release);

            for (int i = 0; i < 5 && !sampler_stop_.load(std::memory_order_relaxed); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }

    void apply_sample(int64_t now_us) noexcept {
        const uint64_t s0 = sample_seq_.load(std::memory_order_acquire);
        if ((s0 & 1u) != 0 || s0 == sample_seen_) return;
        const SamplerPost p = sample_post_;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sample_seq_.load(std::memory_order_relaxed) != s0) return;
        sample_seen_ = s0;
        host_ = p.host;
        if (!p.health_ok) return;
        health_ = p.health;
        pitlane::health_eval(health_, health_limits_, health_st_);
        // One alarm per upward transition; recovery clears silently.
        if (health_st_.level > last_health_level_) {
            (void)alarms_.push(pitlane::kAlarmLink,
                               health_st_.level >= 2 ? pitlane::kAlarmCritical : pitlane::kAlarmWarn,
                               paper_ ? pitlane::kEnvTest : pitlane::kEnvLive, now_us,
                               "host %s bits %u cpu %d mem %d disk %d fds %d",
                               health_st_.level >= 2 ? "critical" : "warn",
                               static_cast<unsigned>(health_st_.bits), health_.cpu_ppm, health_.mem_ppm,
                               health_.disk_used_ppm, health_.fds);
        }
        last_health_level_ = health_st_.level;
    }

    int on_beat(pitlane::FullFlow& flow, int64_t now_us) noexcept {
        pitlane::FlowCounters c{};
        flow.counters(c);
        pitlane::health_beat(now_us);
        apply_sample(now_us);
        last_beat_us_.store(SimOps::now_us(), std::memory_order_relaxed);

        const int64_t equity = equity_base_ + c.realized + c.unrealized;
        if (equity > peak_equity_) peak_equity_ = equity;

        pitlane::RiskInputs rin{};
        rin.equity = equity;
        rin.peak_equity = peak_equity_;
        rin.daily_pnl = c.realized + c.unrealized;
        rin.order_count_1s = c.rate_1s;
        rin.rejects = c.rejects + c.rms_reject;
        rin.orders = c.submitted + c.rms_reject;
        rin.now_us = now_us;
        rin.last_tick_us = paper_ ? now_us : c.last_tick_us;
        rin.md_up = paper_ ? true : (c.last_tick_us > 0 && now_us - c.last_tick_us <= pol_.stale_tick_us);
        rin.td_up = true;

        pitlane::StatusSnapshot snap{};
        (void)cp_.on_beat(now_us, rin, snap);

        if (now_us - last_risk_us_ >= 1000000) {
            last_risk_us_ = now_us;
            pitlane::RiskEvent ev{};
            uint64_t r0 = 0;
            uint64_t r1 = 0;
            if (scale_ok_) (void)pitlane::cycles_ordered(r0);
            const pitlane::RiskAction act = guard_.evaluate(rin, ev);
            cp_.apply_risk(act, ev, now_us);
            if (scale_ok_) {
                (void)pitlane::cycles_ordered(r1);
                hops_.note(pitlane::kHopRisk, ns_of(r0, r1));
            }
            if ((act == pitlane::RiskAction::BlockNew || act == pitlane::RiskAction::Halt) &&
                ev.code != last_risk_code_) {
                last_risk_code_ = ev.code;
                pitlane::OpsAuditRec rec{};
                rec.seq = ch_.alloc_seq();
                rec.code = ev.code;
                rec.kind = 0xff;
                rec.stamp_us = now_us;
                ch_.audit(rec);
                (void)alarms_.push(pitlane::kAlarmRisk, pitlane::kAlarmWarn,
                                   paper_ ? pitlane::kEnvTest : pitlane::kEnvLive, now_us,
                                   "risk action %u code %u", static_cast<unsigned>(act),
                                   static_cast<unsigned>(ev.code));
            } else if (act == pitlane::RiskAction::Ok) {
                last_risk_code_ = 0;
            }
        }

        // 逐跳计时覆盖控制台 beat 循环：行情维护 → 策略 → 清结算 → 快照发布。
        uint64_t c0 = 0;
        uint64_t c1 = 0;
        uint64_t c2 = 0;
        uint64_t c3 = 0;
        uint64_t c4 = 0;
        const bool tm = scale_ok_;
        if (tm) (void)pitlane::cycles_ordered(c0);
        const int fb = tm ? flame_.enter("beat", ns_abs(c0)) : -1;
        int fs = tm ? flame_.enter("market", ns_abs(c0)) : -1;
        watch_market(now_us);
        if (tm) {
            (void)pitlane::cycles_ordered(c1);
            flame_.leave(fs, ns_abs(c1));
            fs = flame_.enter("strategy", ns_abs(c1));
        }
        drain_strategies(now_us);
        run_strategies(*flow_, now_us);
        if (tm) {
            (void)pitlane::cycles_ordered(c2);
            flame_.leave(fs, ns_abs(c2));
            fs = flame_.enter("clear", ns_abs(c2));
        }
        drain_cash(now_us);
        refresh_clearing(now_us);
        if (ledger_poll_ && now_us - last_ledger_us_ >= 5000000) {
            last_ledger_us_ = now_us;
            ledger_poll_(ledger_user_);
        }
        if (tm) {
            (void)pitlane::cycles_ordered(c3);
            flame_.leave(fs, ns_abs(c3));
            fs = flame_.enter("publish", ns_abs(c3));
        }
        fill(snap, flow, c, now_us);
        board_.publish(snap);
        if (tm) {
            (void)pitlane::cycles_ordered(c4);
            flame_.leave(fs, ns_abs(c4));
            flame_.leave(fb, ns_abs(c4));
            hops_.note(pitlane::kHopBook, ns_of(c0, c1));
            hops_.note(pitlane::kHopSignal, ns_of(c1, c2));
            hops_.note(pitlane::kHopEncode, ns_of(c2, c3));
            hops_.note(pitlane::kHopWireOut, ns_of(c3, c4));
            hops_.note_total(ns_of(c0, c4));
        }
        // 日志索引只在 HTTP 线程写，避免交易线程与检索线程同时改环形缓冲。
        return 0;
    }

    int64_t mark_of(uint32_t name) const noexcept {
        for (int i = 0; i < series_.count(); ++i) {
            if (series_.name_at(i) != name) continue;
            const pitlane::SeriesRing<2048>& ring = series_.ring_at(i);
            if (ring.size() == 0) return 0;
            return ring.at(ring.size() - 1).last;
        }
        return 0;
    }

    // ---- strategy instances (trading thread) ----

    static void feed_strat(StratRt& rt, const pitlane::SeriesTick& t) noexcept {
        int64_t mid = t.last;
        if (t.bid > 0 && t.ask > 0) mid = (t.bid + t.ask) / 2;
        if (mid <= 0) return;
        (void)rt.eng.on_sample(mid, t.ts_us);
        (void)rt.eng.on_quote(t.bid, t.ask, t.bid_qty, t.ask_qty, t.ts_us);
    }

    static void config_strat(StratRt& rt, const pitlane::StratSpec& s) noexcept {
        if (s.kind == pitlane::kStratPairKind) {
            pitlane::PairCfg pc{};
            pc.name_a = s.instrument;
            pc.name_b = s.instrument_b;
            pc.acct = s.acct;
            pc.qty_a = s.qty;
            pc.ratio_ppm = s.ratio_ppm;
            pc.window = s.window;
            pc.entry_z_ppm = s.threshold_ppm;
            pc.exit_z_ppm = s.threshold_ppm / 4;
            pc.stop_z_ppm = s.threshold_ppm * 2;
            pc.cooldown_us = s.cooldown_us;
            pc.leg_timeout_us = 2000000;
            pc.max_stale_us = 5000000;
            pc.max_skew_us = 500000;
            pc.max_hold_us = 60000000;
            (void)rt.pair.open(pc);
            return;
        }
        rt.eng = pitlane::FactorEngine<1, 256>{};
        pitlane::FactorSpec fs{};
        fs.kind = s.kind;
        fs.window = s.window;
        (void)rt.eng.add(fs);
    }

    static void feed_pair(StratRt& rt, uint8_t leg, const pitlane::SeriesTick& t) noexcept {
        pitlane::PairTick q{};
        q.bid = t.bid;
        q.ask = t.ask;
        q.ts_us = t.ts_us;
        rt.pair.on_quote(leg, q);
    }

    void reset_strat(int slot, int64_t now) noexcept {
        StratRt& rt = strat_rt_[slot];
        rt = StratRt{};
        rt.started_us = now;
        const pitlane::StratSpec& s = strategies_.spec(slot);
        config_strat(rt, s);
        if (s.kind == pitlane::kStratPairKind) {
            link_backfill(rt, 0, s.instrument);
            link_backfill(rt, 1, s.instrument_b);
            return;
        }
        pitlane::SeriesRing<2048>* ring = series_.find(s.instrument);
        if (!ring) return;
        const int size = ring->size();
        const int from = size > 256 ? size - 256 : 0;
        for (int i = from; i < size; ++i) feed_strat(rt, ring->at(i));
        rt.cursor = size;
    }

    void link_backfill(StratRt& rt, uint8_t leg, uint32_t name) noexcept {
        pitlane::SeriesRing<2048>* ring = series_.find(name);
        if (!ring) return;
        const int size = ring->size();
        const int from = size > 256 ? size - 256 : 0;
        for (int i = from; i < size; ++i) feed_pair(rt, leg, ring->at(i));
        (leg == 0 ? rt.cursor : rt.cursor_b) = size;
    }

    // Applies queued instance commands; only this thread touches the registry state.
    void drain_strategies(int64_t now) noexcept {
        pitlane::StratCmd c{};
        bool changed = false;
        while (strategies_.take(c)) {
            const int slot = strategies_.apply(c, now);
            if (slot < 0) continue;
            if (c.kind == pitlane::kStratCmdStart) reset_strat(slot, now);
            else if (c.kind == pitlane::kStratCmdRemove) strat_rt_[slot] = StratRt{};
            changed = true;
        }
        if (changed) publish_strategies(now);
    }

    void run_strategies(pitlane::FullFlow& flow, int64_t now) noexcept {
        const bool can_order = cp_.allow_new_orders();
        for (int slot = 0; slot < pitlane::StrategyRegistry::kMaxStrategies; ++slot) {
            if (!strategies_.used(slot)) continue;
            const uint8_t st = strategies_.state(slot);
            if (st != pitlane::kStratRunning && st != pitlane::kStratPaused) continue;
            const pitlane::StratSpec& s = strategies_.spec(slot);
            StratRt& rt = strat_rt_[slot];
            if (s.kind == pitlane::kStratPairKind) {
                run_pair(rt, s, flow, now, st, can_order);
                continue;
            }
            pitlane::SeriesRing<2048>* ring = series_.find(s.instrument);
            if (!ring) continue;
            const int size = ring->size();
            if (size < rt.cursor) {
                rt.cursor = 0;
                config_strat(rt, s);
            }
            while (rt.cursor < size) {
                feed_strat(rt, ring->at(rt.cursor));
                ++rt.cursor;
            }
            if (size == 0) continue;
            pitlane::FactorValue fv{};
            if (!rt.eng.value(0, fv) || !fv.ready) continue;
            rt.last_factor = fv.value;
            rt.position = flow.net_qty(s.acct, s.instrument);
            if (st != pitlane::kStratRunning || !can_order) continue;
            const int64_t dir = fv.value * static_cast<int64_t>(s.sign);
            if (dir < s.threshold_ppm && dir > -s.threshold_ppm) continue;
            if (now - rt.last_sig_us < s.cooldown_us) continue;
            const int64_t desired = dir > 0 ? s.qty : -s.qty;
            int64_t delta = desired - rt.position;
            if (delta == 0) continue;
            if (delta > s.qty) delta = s.qty;
            if (delta < -s.qty) delta = -s.qty;
            const pitlane::SeriesTick t = ring->at(size - 1);
            int64_t px = delta > 0 ? t.ask : t.bid;
            if (px <= 0) px = t.last;
            if (px <= 0) continue;
            const int64_t sec = now / 1000000;
            pitlane::Intent in{};
            in.tag = flow.mint().next(static_cast<int>(sec / 86400), static_cast<int>(sec % 86400));
            if (in.tag == 0) {
                ++rt.rejects;
                continue;
            }
            in.name = s.instrument;
            in.acct = s.acct;
            in.px = px;
            in.qty = delta > 0 ? delta : -delta;
            in.side = delta > 0 ? 1 : 2;
            in.act = static_cast<uint8_t>(pitlane::Act::Enter);
            in.style = static_cast<uint8_t>(pitlane::Style::Immediate);
            in.stamp_us = now;
            rt.last_sig_us = now;
            ++rt.signals;
            if (flow.submit_manual(in, now) == 0) ++rt.orders;
            else ++rt.rejects;
        }
        if (now - last_strat_pub_us_ >= 250000) publish_strategies(now);
    }

    // 配对价差：两条腿各自只吃新到的 tick，目标数量由内核按比例给出。
    void run_pair(StratRt& rt, const pitlane::StratSpec& s, pitlane::FullFlow& flow, int64_t now, uint8_t st,
                  bool can_order) noexcept {
        pitlane::SeriesRing<2048>* ra = series_.find(s.instrument);
        pitlane::SeriesRing<2048>* rb = series_.find(s.instrument_b);
        if (!ra || !rb) return;
        const int sa = ra->size();
        const int sb = rb->size();
        if (sa < rt.cursor || sb < rt.cursor_b) {
            rt.cursor = 0;
            rt.cursor_b = 0;
            config_strat(rt, s);
        }
        while (rt.cursor < sa) {
            feed_pair(rt, 0, ra->at(rt.cursor));
            ++rt.cursor;
        }
        while (rt.cursor_b < sb) {
            feed_pair(rt, 1, rb->at(rt.cursor_b));
            ++rt.cursor_b;
        }
        if (sa == 0 || sb == 0) return;
        rt.pair.sync_position(0, flow.net_qty(s.acct, s.instrument));
        rt.pair.sync_position(1, flow.net_qty(s.acct, s.instrument_b));
        pitlane::PairReq reqs[pitlane::PairEngine::kMaxReq]{};
        const int n = rt.pair.step(now, reqs, pitlane::PairEngine::kMaxReq);
        const pitlane::PairRow row = rt.pair.row();
        rt.last_factor = row.z_ppm;
        rt.position = row.pos_a;
        rt.signals = row.signals;
        rt.last_sig_us = row.last_sig_us;
        if (st != pitlane::kStratRunning || !can_order) return;
        for (int i = 0; i < n; ++i) {
            const pitlane::PairReq& q = reqs[i];
            if (now < rt.next_emit_us[q.leg]) continue;
            const int64_t sec = now / 1000000;
            pitlane::Intent in{};
            in.tag = flow.mint().next(static_cast<int>(sec / 86400), static_cast<int>(sec % 86400));
            if (in.tag == 0) {
                rt.pair.on_reject(q.leg);
                continue;
            }
            in.name = q.name;
            in.acct = s.acct;
            in.px = q.px;
            in.qty = q.qty;
            in.side = q.side;
            in.act = static_cast<uint8_t>(pitlane::Act::Enter);
            in.style = static_cast<uint8_t>(pitlane::Style::Immediate);
            in.stamp_us = now;
            if (flow.submit_manual(in, now) == 0) {
                rt.next_emit_us[q.leg] = now + 200000;
            } else {
                // 报单被拒：清掉该腿在途量，下一拍按真实持仓重新计算差额。
                rt.pair.on_reject(q.leg);
                rt.next_emit_us[q.leg] = now + 100000;
            }
        }
    }

    int shared_count(uint32_t acct, uint32_t instrument) const noexcept {
        int n = 0;
        for (int i = 0; i < pitlane::StrategyRegistry::kMaxStrategies; ++i) {
            if (!strategies_.used(i)) continue;
            const pitlane::StratSpec& s = strategies_.spec(i);
            if (s.acct == acct && s.instrument == instrument) ++n;
        }
        return n;
    }

    void publish_strategies(int64_t now) noexcept {
        last_strat_pub_us_ = now;
        for (int slot = 0; slot < pitlane::StrategyRegistry::kMaxStrategies; ++slot) {
            if (!strategies_.used(slot)) continue;
            const pitlane::StratSpec& s = strategies_.spec(slot);
            StratRt& rt = strat_rt_[slot];
            pitlane::StratRow row{};
            std::snprintf(row.id, sizeof row.id, "%s", s.id);
            row.state = strategies_.state(slot);
            row.kind = s.kind;
            row.window = s.window;
            row.sign = s.sign;
            row.env = s.env;
            row.acct = s.acct;
            row.instrument = s.instrument;
            row.instrument_b = s.instrument_b;
            row.threshold_ppm = s.threshold_ppm;
            row.qty = s.qty;
            row.ratio_ppm = s.ratio_ppm;
            row.factor = rt.last_factor;
            row.position = rt.position;
            row.signals = rt.signals;
            row.orders = rt.orders;
            row.rejects = rt.rejects;
            row.last_sig_us = rt.last_sig_us;
            row.started_us = rt.started_us;
            row.shared = shared_count(s.acct, s.instrument) > 1 ? 1 : 0;
            bool pair_flat = false;
            if (s.kind == pitlane::kStratPairKind) {
                const pitlane::PairRow pr = rt.pair.row();
                row.factor = pr.z_ppm;
                row.position = pr.pos_a;
                row.signals = pr.signals;
                row.orders = pr.orders;
                row.rejects = pr.rejects + rt.rejects;
                row.last_sig_us = pr.last_sig_us;
                row.position_b = pr.pos_b;
                row.spread = pr.spread;
                row.mean = pr.mean;
                row.sd = pr.sd;
                row.rounds = pr.rounds;
                row.wins = pr.wins;
                row.losses = pr.losses;
                row.timeouts = pr.timeouts;
                row.stop_hits = pr.stop_hits;
                row.pair_ready = pr.ready;
                pair_flat = pr.pos_a == 0 && pr.pos_b == 0;
                // 配对回合盈亏由内核按双腿成本结算，账户持仓的已实现盈亏在纸面环境不维护。
                row.realized = pr.realized;
                std::snprintf(row.note, sizeof row.note, "%s", pitlane::pair_state_name(pr.state));
            } else {
                std::snprintf(row.note, sizeof row.note, "%s", strategies_.note(slot));
            }
            if (flow_ && !pair_flat) {
                const bool pair = s.kind == pitlane::kStratPairKind;
                if (const pitlane::PmsPos* p = flow_->pms().find(s.acct, s.instrument)) {
                    if (!pair) row.realized = p->realized;
                    const int64_t mark = mark_of(s.instrument);
                    int64_t u = 0;
                    if (mark > 0 && flow_->pms().unrealized(s.acct, s.instrument, mark, u)) row.unrealized = u;
                }
                if (pair) {
                    if (const pitlane::PmsPos* pb = flow_->pms().find(s.acct, s.instrument_b)) {
                        const int64_t mark_b = mark_of(s.instrument_b);
                        int64_t ub = 0;
                        if (mark_b > 0 && flow_->pms().unrealized(s.acct, s.instrument_b, mark_b, ub)) {
                            row.unrealized += ub;
                        }
                    }
                }
            }
            strategies_.publish(slot, row);
        }
    }

    // strategy = <id> <acct_id> <instrument> <kind> <window> <sign> <threshold_ppm> <qty> <cooldown_ms>
    //            [test|live] [b=<instrument_b> ratio=<ppm>] [auto]
    // `auto` 让实例在控制台启动后的第一个交易节拍自动进入 running，便于随进程部署策略。
    void load_strategies() noexcept {
        std::FILE* f = std::fopen(strat_path_, "r");
        if (!f) return;
        char line[256];
        int loaded = 0;
        while (std::fgets(line, sizeof line, f)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p || *p == '\n' || *p == '\r' || *p == '#') continue;
            char* eq = std::strchr(p, '=');
            if (!eq) continue;
            char* ke = eq;
            while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) --ke;
            *ke = 0;
            if (std::strcmp(p, "strategy") != 0) continue;
            char* val = eq + 1;
            char id[24]{};
            char acct_tok[24]{};
            char env_tok[8]{};
            unsigned inst = 0;
            unsigned kind = 0;
            unsigned win = 0;
            int sign = 1;
            long long thr = 0;
            long long qty = 0;
            long long cd_ms = 0;
            const int n = std::sscanf(val, "%23s %23s %u %u %u %d %lld %lld %lld %7s", id, acct_tok, &inst,
                                      &kind, &win, &sign, &thr, &qty, &cd_ms, env_tok);
            if (n < 9) {
                std::printf("[ops] strategies: bad line near %s\n", id[0] ? id : "?");
                continue;
            }
            pitlane::StratSpec s{};
            std::snprintf(s.id, sizeof s.id, "%s", id);
            s.acct = accs_.resolve(acct_tok);
            s.instrument = inst;
            s.kind = static_cast<uint8_t>(kind);
            s.window = static_cast<uint16_t>(win);
            s.sign = static_cast<int8_t>(sign < 0 ? -1 : 1);
            s.threshold_ppm = thr;
            s.qty = qty;
            s.cooldown_us = cd_ms * 1000;
            s.env = (n >= 10 && std::strcmp(env_tok, "live") == 0) ? pitlane::kEnvLive : pitlane::kEnvTest;
            if (const char* bp = std::strstr(val, "b=")) {
                s.instrument_b = static_cast<uint32_t>(std::strtoull(bp + 2, nullptr, 10));
            }
            if (const char* rp = std::strstr(val, "ratio=")) {
                s.ratio_ppm = static_cast<int64_t>(std::strtoll(rp + 6, nullptr, 10));
            }
            if (strategies_.apply_add(s, now_us()) >= 0) {
                ++loaded;
                if (std::strstr(val, "auto") != nullptr) {
                    pitlane::StratCmd c{};
                    c.kind = pitlane::kStratCmdStart;
                    c.env = s.env;
                    std::snprintf(c.id, sizeof c.id, "%s", s.id);
                    (void)strategies_.submit(c);
                }
            } else {
                std::printf("[ops] strategies: rejected %s\n", id);
            }
        }
        std::fclose(f);
        if (loaded) std::printf("[ops] loaded %d strategy instance(s) from %s\n", loaded, strat_path_);
    }

    // Safety ladder for market data: a warning at two thirds of the jump (or
    // stale) threshold, and at the threshold pull every order of the product
    // across the deployment and stop its trading gateway. Replays are exempt.
    void watch_market(int64_t now) noexcept {
        if (paper_ || (pol_.max_jump_ppm <= 0 && pol_.stale_tick_us <= 0)) return;
        if (now - last_watch_us_ < 1000000) return;
        last_watch_us_ = now;
        for (int i = 0; i < series_.count(); ++i) {
            const pitlane::SeriesRing<2048>& ring = series_.ring_at(i);
            const uint32_t name = series_.name_at(i);
            if (ring.size() == 0) continue;
            const pitlane::SeriesTick last = ring.at(ring.size() - 1);
            int64_t ppm = 0;
            if (ring.size() >= 2) {
                const pitlane::SeriesTick prev = ring.at(ring.size() - 2);
                if (prev.last > 0) ppm = (last.last - prev.last) * 1000000 / prev.last;
            }
            const int64_t abs_ppm = ppm < 0 ? -ppm : ppm;
            uint8_t sev = 0;
            bool stale = false;
            if (pol_.max_jump_ppm > 0) {
                if (abs_ppm >= pol_.max_jump_ppm) sev = 2;
                else if (abs_ppm >= pol_.max_jump_ppm * 2 / 3) sev = 1;
            }
            if (pol_.stale_tick_us > 0 && last.ts_us > 0) {
                const int64_t age = now - last.ts_us;
                if (age > pol_.stale_tick_us * 2) {
                    sev = 2;
                    stale = true;
                } else if (age > pol_.stale_tick_us && sev < 1) {
                    sev = 1;
                    stale = true;
                }
            }
            const int slot = i < 32 ? i : 31;
            if (sev <= watch_sev_[slot]) {
                watch_sev_[slot] = sev;
                continue;
            }
            watch_sev_[slot] = sev;
            if (sev == 2) {
                const int pulled = flow_ ? flow_->cancel_name(name, now) : 0;
                (void)alarms_.push(stale ? pitlane::kAlarmStale : pitlane::kAlarmJump, pitlane::kAlarmCritical,
                                   td_env_of(name), now, "name %u broken: pulled %d orders, stopping td",
                                   static_cast<unsigned>(name), pulled);
                audit_risk(sev, now);
                stop_td_for_name(name, now);
            } else {
                (void)alarms_.push(stale ? pitlane::kAlarmStale : pitlane::kAlarmJump, pitlane::kAlarmWarn,
                                   td_env_of(name), now, "name %u near threshold: %lld ppm",
                                   static_cast<unsigned>(name), static_cast<long long>(ppm));
            }
        }
    }

    // Mode tag for a market alarm: the instrument's own trading gateway when set.
    uint8_t td_env_of(uint32_t name) const noexcept {
        const pitlane::InstrSpec* ins = instr_.by_name(name);
        if (ins && ins->gw[0]) {
            const int g = gw_.index_of(ins->gw);
            if (g >= 0) return gw_.spec(g).env_bit;
        }
        return pitlane::kEnvLive;
    }

    static void clearing_cb(void* user, uint32_t acct, uint32_t name, uint8_t side, int64_t qty, int64_t px) noexcept {
        static_cast<SimOps*>(user)->on_clearing(acct, name, side, qty, px);
    }

    void on_clearing(uint32_t acct, uint32_t name, uint8_t side, int64_t qty, int64_t px) noexcept {
        const int bi = accs_.index_of_num(acct);
        if (bi < 0 || bi >= 8) return;
        pitlane::ClrFillIn f{};
        f.acct = acct;
        f.name = name;
        f.side = side;
        f.qty = qty;
        f.px = px;
        if (const pitlane::InstrSpec* ins = instr_.by_name(name)) {
            f.mult = ins->mult;
            f.margin_ppm = ins->margin_ppm;
            f.fee_ppm = ins->fee_ppm;
            f.close_fee_ppm = ins->close_fee_ppm;
            f.close_today_fee_ppm = ins->close_today_fee_ppm;
            f.stamp_ppm = ins->stamp_ppm;
            f.min_fee = ins->min_fee;
            f.allow_short = ins->allow_short;
            const int ci = books_[bi].ccy_id(ins->ccy);
            if (ci >= 0) {
                f.ccy = static_cast<uint8_t>(ci);
                if (ins->fx_ppm > 0) books_[bi].set_rate(f.ccy, ins->fx_ppm);
            }
        }
        (void)books_[bi].apply(f, now_us());
    }

    int build_marks(pitlane::ClrMark* marks, int cap) const noexcept {
        int n = 0;
        for (int k = 0; k < series_.count() && n < cap; ++k) {
            const uint32_t name = series_.name_at(k);
            const int64_t px = mark_of(name);
            marks[n].name = name;
            marks[n].last = px;
            marks[n].settle = px;
            ++n;
        }
        return n;
    }

    void ingest_ledger(int kind, const pitlane::LedgerAsset* assets, int n_assets,
                       const pitlane::LedgerPosition* positions, int n_positions, int64_t stamp_us) noexcept {
        if (kind == 1) ledger_.replace_assets(assets, n_assets, stamp_us);
        else if (kind == 2) ledger_.replace_positions(positions, n_positions, stamp_us);
    }

    // Paper has no counter feed, so the clearing report stands in for one. The
    // rows use the same units as the CTP feed: whole yuan and 1e-4 prices.
    void fill_ledger_from_clearing(int64_t now) noexcept {
        static constexpr int kMaxA = 8;
        static constexpr int kMaxP = 32;
        pitlane::LedgerAsset assets[kMaxA]{};
        pitlane::LedgerPosition positions[kMaxP]{};
        int na = 0;
        int np = 0;
        for (int i = 0; i < accs_.count() && i < 8; ++i) {
            pitlane::ClearingReport rep{};
            if (!clear_board_[i].read(rep)) continue;
            if (na < kMaxA) {
                pitlane::LedgerAsset& a = assets[na++];
                a = pitlane::LedgerAsset{};
                a.tier = static_cast<uint8_t>(pitlane::LedgerTier::Trader);
                a.acct = rep.acct;
                a.initial_equity = rep.cash_in;
                a.static_equity = rep.pre_equity;
                a.dynamic_equity = rep.equity;
                a.total_cash = rep.cash_balance;
                a.available_cash = rep.available;
                a.market_value = rep.market_value;
                a.margin = rep.margin;
                a.accumulated_fee = rep.commission;
                a.intraday_fee = rep.day_fees;
                a.realized_pnl = rep.realized;
                a.unrealized_pnl = rep.unrealized;
                a.position_pnl = rep.unrealized;
                a.close_pnl = rep.realized;
                a.asset_balance = rep.equity;
                a.fetch_balance = rep.available;
                a.send_us = now;
                a.stamp_us = now;
            }
            for (int r = 0; r < rep.rows && r < 8; ++r) {
                const pitlane::ClearingRow& row = rep.row[r];
                const int64_t total = row.long_qty + row.short_qty;
                const auto share = [total](int64_t v, int64_t qty) noexcept {
                    return total > 0 ? v * qty / total : 0;
                };
                if (row.long_qty > 0 && np < kMaxP) {
                    pitlane::LedgerPosition& p = positions[np++];
                    p = pitlane::LedgerPosition{};
                    p.tier = static_cast<uint8_t>(pitlane::LedgerTier::Trader);
                    p.acct = rep.acct;
                    p.product = row.name;
                    p.name = row.name;
                    p.direction = static_cast<uint8_t>(pitlane::LedgerSide::Long);
                    p.total = row.long_qty;
                    p.initial_yesterday = row.long_yday;
                    p.left_yesterday = row.long_yday;
                    p.sellable = row.long_qty;
                    p.yesterday_buy = row.long_yday;
                    p.today_buy = row.long_today;
                    p.avg_open_px = row.avg_long;
                    p.cost_px = row.avg_long;
                    p.last_px = row.mark;
                    p.settlement_px = row.settle_mark;
                    p.margin = share(row.margin, row.long_qty);
                    p.position_pnl = share(row.unrealized, row.long_qty);
                    p.unrealized_pnl = p.position_pnl;
                    p.close_pnl = share(row.realized, row.long_qty);
                    p.realized_pnl = p.close_pnl;
                    p.commission = share(row.commission, row.long_qty);
                    p.stamp_us = now;
                }
                if (row.short_qty > 0 && np < kMaxP) {
                    pitlane::LedgerPosition& p = positions[np++];
                    p = pitlane::LedgerPosition{};
                    p.tier = static_cast<uint8_t>(pitlane::LedgerTier::Trader);
                    p.acct = rep.acct;
                    p.product = row.name;
                    p.name = row.name;
                    p.direction = static_cast<uint8_t>(pitlane::LedgerSide::Short);
                    p.total = row.short_qty;
                    p.initial_yesterday = row.short_yday;
                    p.left_yesterday = row.short_yday;
                    p.sellable = row.short_qty;
                    p.yesterday_sell = row.short_yday;
                    p.today_sell = row.short_today;
                    p.avg_open_px = row.avg_short;
                    p.cost_px = row.avg_short;
                    p.last_px = row.mark;
                    p.settlement_px = row.settle_mark;
                    p.margin = share(row.margin, row.short_qty);
                    p.position_pnl = share(row.unrealized, row.short_qty);
                    p.unrealized_pnl = p.position_pnl;
                    p.close_pnl = share(row.realized, row.short_qty);
                    p.realized_pnl = p.close_pnl;
                    p.commission = share(row.commission, row.short_qty);
                    p.stamp_us = now;
                }
            }
        }
        ledger_.replace(assets, na, positions, np, now);
    }

    void refresh_clearing(int64_t now, bool force = false) noexcept {
        if (!force && now - last_clear_us_ < 1000000) return;
        last_clear_us_ = now;
        pitlane::ClrMark marks[8]{};
        const int n = build_marks(marks, 8);
        for (int i = 0; i < accs_.count() && i < 8; ++i) {
            pitlane::ClearingReport rep{};
            books_[i].report(marks, n, accs_.spec(i).env, rep);
            if (flow_) (void)books_[i].reconcile(flow_->pms(), accs_.spec(i).num, rep);
            rep.seq = ++clear_seq_;
            rep.stamp_us = now;
            clear_board_[i].publish(rep);
        }
        if (!ledger_poll_) fill_ledger_from_clearing(now);
    }

    // End-of-day. The snapshot carries the day's numbers, positions are marked
    // to the settlement price, today's lots become yesterday's, and the daily
    // counters restart while cash, PnL, and positions carry.
    void settle_clearing(int64_t now, uint8_t env) noexcept {
        pitlane::ClrMark marks[8]{};
        const int n = build_marks(marks, 8);
        for (int i = 0; i < accs_.count() && i < 8; ++i) {
            if (accs_.spec(i).env != env) continue;
            books_[i].settle(now, marks, n);
        }
        refresh_clearing(now, true);
    }

    // Book requests arrive on the HTTP thread; the trading thread owns the
    // clearing books, so they travel through this single-producer queue. Kinds
    // 1-2 are the cash ledger and 3-7 are corporate actions, accruals, and
    // share movements.
    enum BookJobKind : uint8_t {
        kJobDeposit = 1,
        kJobWithdraw = 2,
        kJobCorp = 3,
        kJobExpense = 4,
        kJobInterest = 5,
        kJobSubscribe = 6,
        kJobRedeem = 7,
    };

    struct CashJob {
        uint32_t acct{0};
        uint8_t kind{0};
        char ccy[8]{};
        int64_t amount{0};
        int64_t seq{0};
        uint32_t name{0};
        uint8_t corp_kind{0};
        uint8_t expense_kind{0};
        int64_t event_id{0};
        int64_t per10_cash{0};
        int64_t per10_stock{0};
        int64_t num{1};
        int64_t den{1};
        int64_t tax_ppm{0};
    };

    bool enqueue_book(CashJob& src) noexcept {
        const uint32_t head = cash_head_.load(std::memory_order_relaxed);
        const uint32_t tail = cash_tail_.load(std::memory_order_acquire);
        if (head - tail >= static_cast<uint32_t>(kCashQ)) return false;
        src.seq = ++cash_req_seq_;
        cash_q_[head % kCashQ] = src;
        cash_head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool submit_cash(uint32_t acct, uint8_t kind, const char* ccy, int64_t amount, int64_t& seq) noexcept {
        CashJob j{};
        j.acct = acct;
        j.kind = kind;
        std::snprintf(j.ccy, sizeof j.ccy, "%s", ccy && ccy[0] ? ccy : "");
        j.amount = amount;
        if (!enqueue_book(j)) return false;
        seq = cash_req_seq_;
        return true;
    }

    void drain_cash(int64_t now) noexcept {
        uint32_t tail = cash_tail_.load(std::memory_order_relaxed);
        const uint32_t head = cash_head_.load(std::memory_order_acquire);
        while (tail != head) {
            const CashJob j = cash_q_[tail % kCashQ];
            const int bi = accs_.index_of_num(j.acct);
            if (bi >= 0 && bi < 8) {
                pitlane::ClearingBook& book = books_[bi];
                int64_t ack = -j.seq;
                switch (j.kind) {
                    case kJobDeposit:
                    case kJobWithdraw: {
                        const int ci = book.ccy_id(j.ccy);
                        const uint8_t ccy = ci >= 0 ? static_cast<uint8_t>(ci) : 0;
                        const pitlane::ClrCashMove m = j.kind == pitlane::kClrWithdraw
                                                           ? book.withdraw(j.amount, ccy, now)
                                                           : book.deposit(j.amount, ccy, now);
                        ack = m.seq > 0 ? m.seq : -j.seq;
                        break;
                    }
                    case kJobCorp: {
                        pitlane::ClrCorp c{};
                        c.name = j.name;
                        c.event_id = j.event_id;
                        c.kind = j.corp_kind;
                        c.per10_cash = j.per10_cash;
                        c.per10_stock = j.per10_stock;
                        c.num = j.num;
                        c.den = j.den;
                        c.tax_ppm = j.tax_ppm;
                        const pitlane::ClrCorpOut r = book.apply_corp(c, now);
                        ack = r.applied ? j.seq : -j.seq;
                        break;
                    }
                    case kJobExpense:
                        book.accrue_expense(j.expense_kind, j.amount, now);
                        ack = j.seq;
                        break;
                    case kJobInterest:
                        book.accrue_interest(j.amount, now);
                        ack = j.seq;
                        break;
                    case kJobSubscribe:
                        ack = book.subscribe(j.amount, now) ? j.seq : -j.seq;
                        break;
                    case kJobRedeem:
                        ack = book.redeem(j.amount, now) ? j.seq : -j.seq;
                        break;
                    default:
                        break;
                }
                cash_ack_.store(ack);
            }
            ++tail;
            cash_tail_.store(tail, std::memory_order_release);
        }
    }


    void fill(pitlane::StatusSnapshot& snap, pitlane::FullFlow& flow, const pitlane::FlowCounters& c,
              int64_t now_us) noexcept {
        snap.seq = ++snap_seq_;
        snap.stamp_us = now_us;
        snap.last_tick_us = paper_ ? now_us : c.last_tick_us;
        // Uptime runs on the wall clock; `now_us` is the event stamp and may be
        // simulated time in paper, so subtracting it gives zero there.
        const int64_t wall = SimOps::now_us();
        snap.uptime_us = wall > start_us_ ? wall - start_us_ : 0;
        snap.quotes = c.quotes;
        snap.signals = c.parents_closed;
        snap.parents = c.parents_closed;
        snap.child_orders = c.submitted;
        snap.fills = c.fills;
        snap.rejects = c.rejects + c.rms_reject;
        snap.cancels = c.cancels;
        snap.net_qty = 0;
        snap.realized = c.realized;
        snap.unrealized = c.unrealized;
        snap.equity = equity_base_ + c.realized + c.unrealized;
        snap.peak_equity = peak_equity_;
        snap.drawdown = peak_equity_ > snap.equity ? peak_equity_ - snap.equity : 0;
        snap.turnover = c.daily_notional;
        snap.fees = c.fees;
        snap.order_rate_1s = c.rate_1s;
        const int64_t all = c.submitted + c.rms_reject;
        snap.reject_rate_ppm = all > 0 ? (c.rejects + c.rms_reject) * 1000000 / all : 0;
        if (hooks_.timeouts) {
            int64_t ack = 0;
            int64_t life = 0;
            int64_t pull = 0;
            hooks_.timeouts(hooks_.user, &ack, &life, &pull);
            snap.ack_timeouts = ack;
            snap.life_timeouts = life;
            snap.pull_timeouts = pull;
        }
        snap.cpu_ppm = host_.cpu_ppm;
        snap.mem_ppm = host_.mem_ppm;
        snap.load_1m_ppm = host_.load_1m_ppm;
        snap.threads = host_.threads;
        snap.fds = health_.fds;
        snap.disk_used_ppm = health_.disk_used_ppm;
        snap.rx_bps = health_.rx_bps;
        snap.tx_bps = health_.tx_bps;
        snap.host_uptime_s = health_.uptime_s;
        snap.last_beat_us = last_beat_us_.load(std::memory_order_relaxed);
        snap.health_level = health_st_.level;
        snap.health_bits = health_st_.bits;
        const pitlane::RmsLimits& rl = flow.rms().limits();
        snap.risk.max_order_qty = rl.max_order_qty;
        snap.risk.max_order_notional = rl.max_order_notional;
        snap.risk.max_pos_long = rl.max_pos_long;
        snap.risk.max_pos_short = rl.max_pos_short;
        snap.risk.max_daily_notional = rl.max_daily_notional;
        snap.risk.price_band_ppm = rl.price_band_ppm;
        snap.risk.tick_size = rl.tick_size;
        snap.risk.lot_size = rl.lot_size;
        snap.risk.max_orders_per_sec = rl.max_orders_per_sec;
        snap.risk.max_live_orders = rl.max_live_orders;
        snap.risk.halted = flow.rms().halted() ? 1 : 0;
        snap.risk.precheck = flow.precheck() ? 1 : 0;

        int np = 0;
        const pitlane::Pms<64>& pms = flow.pms();
        for (int i = 0; i < pms.size() && np < pitlane::kStatusPositions; ++i) {
            const pitlane::PmsPos& p = pms.at(i);
            pitlane::StatusPosition& s = snap.pos[np++];
            s.acct = p.acct;
            s.name = p.name;
            s.today = p.long_qty - p.short_qty;
            s.yday = p.long_yday - p.short_yday;
            s.avg_px = p.long_qty > 0 ? p.avg_long_px : p.avg_short_px;
            s.realized = p.realized;
            s.unrealized = 0;
            const int64_t mark = mark_of(p.name);
            if (mark > 0) (void)pms.unrealized(p.acct, p.name, mark, s.unrealized);
        }
        snap.pos_n = np;
        snap.net_qty = 0;
        for (int i = 0; i < np; ++i) snap.net_qty += snap.pos[i].today;
    }

    // ---- JSON rendering -------------------------------------------------

    struct Out {
        char* buf;
        int cap;
        int n;
        void add(const char* fmt, ...) noexcept {
            if (n >= cap) return;
            va_list ap;
            va_start(ap, fmt);
            const int w = std::vsnprintf(buf + n, static_cast<std::size_t>(cap - n), fmt, ap);
            va_end(ap);
            if (w > 0) n += w < cap - n ? w : cap - n - 1;
        }
    };

    // ---- 席位台账与性能基线 ---------------------------------------------

    // 每个网关对应一个席位，通道取网关适配器名。运行中的网关席位直接置为 active。
    void seed_seats() noexcept {
        seats_.clear();
        const uint8_t env = paper_ ? pitlane::kEnvTest : pitlane::kEnvLive;
        for (int i = 0; i < gw_.count(); ++i) {
            const pitlane::GwSpec& g = gw_.spec(i);
            if (g.env_bit != env) continue;
            pitlane::SeatSpec s{};
            s.id = static_cast<uint32_t>(i + 1);
            std::snprintf(s.name, sizeof s.name, "%s", g.id);
            std::snprintf(s.broker, sizeof s.broker, "%s", g.acct);
            std::snprintf(s.exchange, sizeof s.exchange, "%s", g.adapter);
            std::snprintf(s.channel, sizeof s.channel, "%s", g.adapter[0] ? g.adapter : "default");
            s.enabled = g.available ? 1 : 0;
            const int idx = seats_.add(s);
            if (idx >= 0 && gw_.status(i).state == pitlane::kGwRunning) (void)seats_.start(s.id, now_us());
        }
        // 仿真模式没有真实网关，仍然给出一条 paper 通道，席位视图不至于空着。
        if (seats_.count() == 0) {
            pitlane::SeatSpec s{};
            s.id = 1;
            std::snprintf(s.name, sizeof s.name, "%s", "paper");
            std::snprintf(s.broker, sizeof s.broker, "%s", "-");
            std::snprintf(s.exchange, sizeof s.exchange, "%s", "paper");
            std::snprintf(s.channel, sizeof s.channel, "%s", "paper");
            s.enabled = 1;
            if (seats_.add(s) >= 0) (void)seats_.start(s.id, now_us());
        }
    }

    // 按通道轮询一个还有容量的席位；控制台命令不区分通道，取第一个可用的。
    int pick_seat(int64_t now_us) noexcept {
        for (int c = 0; c < seats_.channels(); ++c) {
            const int i = seats_.pick(seats_.channel_name(c), now_us);
            if (i >= 0) return i;
        }
        return -1;
    }

    void load_baseline() noexcept {
        base_.clear();
        baseline_rev_[0] = 0;
        if (!base_path_[0]) return;
        std::FILE* f = std::fopen(base_path_, "r");
        if (!f) return;
        char text[4096];
        const std::size_t n = std::fread(text, 1, sizeof text, f);
        std::fclose(f);
        if (n == 0) return;
        (void)base_.from_text(text, static_cast<int>(n));
        // 第一行注释里带着基线对应的版本，原样回显给控制台。
        const char* p = std::strstr(text, "rev=");
        if (p) {
            p += 4;
            int i = 0;
            while (p[i] && p[i] != ' ' && p[i] != '\n' && i + 1 < static_cast<int>(sizeof baseline_rev_))
                ++i;
            std::memcpy(baseline_rev_, p, static_cast<std::size_t>(i));
            baseline_rev_[i] = 0;
        }
    }

    bool save_baseline() noexcept {
        if (!base_path_[0]) return false;
        char text[4096];
        const int n = base_.to_text(text, sizeof text, pitlane::kVersion, static_cast<int64_t>(std::time(nullptr)));
        if (n <= 0) return false;
        std::FILE* f = std::fopen(base_path_, "w");
        if (!f) return false;
        const bool ok = std::fwrite(text, 1, static_cast<std::size_t>(n), f) == static_cast<std::size_t>(n);
        std::fclose(f);
        if (ok) std::snprintf(baseline_rev_, sizeof baseline_rev_, "%s", pitlane::kVersion);
        return ok;
    }

    // 当前一次运行的延迟画像。行情端与交易端分开命名，避免同名混淆。
    void collect_perf(pitlane::PerfBaseline& out) const noexcept {
        out.clear();
        if (md_hops_) {
            for (int i = 0; i < pitlane::kHopStages; ++i) {
                pitlane::HopStageReport r{};
                if (!md_hops_->report(static_cast<uint8_t>(i), r) || r.n == 0) continue;
                char name[pitlane::kPerfName];
                std::snprintf(name, sizeof name, "md.%s", pitlane::hop_name(static_cast<uint8_t>(i)));
                pitlane::LatReport lr{};
                lr.n = r.n;
                lr.min = r.min_ns;
                lr.p50 = r.p50_ns;
                lr.p99 = r.p99_ns;
                lr.max = r.max_ns;
                (void)out.put(name, lr);
            }
            pitlane::HopStageReport total{};
            if (md_hops_->report_total(total) && total.n > 0) {
                pitlane::LatReport lr{};
                lr.n = total.n;
                lr.min = total.min_ns;
                lr.p50 = total.p50_ns;
                lr.p99 = total.p99_ns;
                lr.max = total.max_ns;
                (void)out.put("md.total", lr);
            }
        }
        for (int i = 0; i < pitlane::kHopStages; ++i) {
            pitlane::HopStageReport r{};
            if (!hops_.report(static_cast<uint8_t>(i), r) || r.n == 0) continue;
            char name[pitlane::kPerfName];
            std::snprintf(name, sizeof name, "ops.%s", pitlane::hop_name(static_cast<uint8_t>(i)));
            pitlane::LatReport lr{};
            lr.n = r.n;
            lr.min = r.min_ns;
            lr.p50 = r.p50_ns;
            lr.p99 = r.p99_ns;
            lr.max = r.max_ns;
            (void)out.put(name, lr);
        }
    }

    static void hop_json(Out& o, const char* group, const pitlane::HopLedger& led) noexcept {
        o.add("\"%s\":{\"stages\":[", group);
        bool first = true;
        for (int i = 0; i < pitlane::kHopStages; ++i) {
            pitlane::HopStageReport r{};
            if (!led.report(static_cast<uint8_t>(i), r) || r.n == 0) continue;
            if (!first) o.add(",");
            first = false;
            o.add("{\"stage\":\"%s\",\"n\":%llu,\"min_ns\":%llu,\"p50_ns\":%llu,\"p99_ns\":%llu,"
                  "\"max_ns\":%llu,\"over\":%llu}",
                  pitlane::hop_name(static_cast<uint8_t>(i)), static_cast<unsigned long long>(r.n),
                  static_cast<unsigned long long>(r.min_ns), static_cast<unsigned long long>(r.p50_ns),
                  static_cast<unsigned long long>(r.p99_ns), static_cast<unsigned long long>(r.max_ns),
                  static_cast<unsigned long long>(r.over));
        }
        o.add("]");
        pitlane::HopStageReport t{};
        if (led.report_total(t) && t.n > 0) {
            o.add(",\"total\":{\"n\":%llu,\"min_ns\":%llu,\"p50_ns\":%llu,\"p99_ns\":%llu,\"max_ns\":%llu,"
                  "\"over\":%llu}",
                  static_cast<unsigned long long>(t.n), static_cast<unsigned long long>(t.min_ns),
                  static_cast<unsigned long long>(t.p50_ns), static_cast<unsigned long long>(t.p99_ns),
                  static_cast<unsigned long long>(t.max_ns), static_cast<unsigned long long>(t.over));
        } else {
            o.add(",\"total\":null");
        }
        o.add("}");
    }

    static void json_escape(Out& o, const char* s, int n) noexcept {
        for (int i = 0; i < n; ++i) {
            const char c = s[i];
            if (c == '"' || c == '\\') o.add("\\%c", c);
            else if (c == '\n') o.add("\\n");
            else if (c == '\r') o.add("\\r");
            else if (c == '\t') o.add("\\t");
            else if (static_cast<unsigned char>(c) >= 0x20) o.add("%c", c);
        }
    }

    void render_status(const pitlane::StatusSnapshot& s, uint8_t env) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"seq\":%llu,\"stamp_us\":%lld,\"state\":%u,\"md_up\":%u,\"td_up\":%u,\"strategy_on\":%u",
              static_cast<unsigned long long>(s.seq), static_cast<long long>(s.stamp_us),
              static_cast<unsigned>(s.state), static_cast<unsigned>(s.md_up), static_cast<unsigned>(s.td_up),
              static_cast<unsigned>(s.strategy_on));
        o.add(",\"halted\":%u,\"version\":%u,\"last_tick_us\":%lld,\"uptime_us\":%lld",
              static_cast<unsigned>(s.halted), static_cast<unsigned>(s.config_version),
              static_cast<long long>(s.last_tick_us), static_cast<long long>(s.uptime_us));
        o.add(",\"counters\":{\"quotes\":%lld,\"signals\":%lld,\"parents\":%lld,\"children\":%lld,\"fills\":%lld,"
              "\"rejects\":%lld,\"cancels\":%lld,\"orders_1s\":%lld,\"reject_ppm\":%lld,"
              "\"ack_timeouts\":%lld,\"life_timeouts\":%lld,\"pull_timeouts\":%lld}",
              static_cast<long long>(s.quotes), static_cast<long long>(s.signals), static_cast<long long>(s.parents),
              static_cast<long long>(s.child_orders), static_cast<long long>(s.fills),
              static_cast<long long>(s.rejects), static_cast<long long>(s.cancels),
              static_cast<long long>(s.order_rate_1s), static_cast<long long>(s.reject_rate_ppm),
              static_cast<long long>(s.ack_timeouts), static_cast<long long>(s.life_timeouts),
              static_cast<long long>(s.pull_timeouts));
        o.add(",\"risk\":{\"net_qty\":%lld,\"realized\":%lld,\"unrealized\":%lld,\"equity\":%lld,\"peak\":%lld,"
              "\"drawdown\":%lld,\"turnover\":%lld,\"fees\":%lld}",
              static_cast<long long>(s.net_qty), static_cast<long long>(s.realized),
              static_cast<long long>(s.unrealized), static_cast<long long>(s.equity),
              static_cast<long long>(s.peak_equity), static_cast<long long>(s.drawdown),
              static_cast<long long>(s.turnover), static_cast<long long>(s.fees));
        o.add(",\"host\":{\"cpu_ppm\":%d,\"mem_ppm\":%d,\"load_ppm\":%d,\"threads\":%d}",
              static_cast<int>(s.cpu_ppm), static_cast<int>(s.mem_ppm), static_cast<int>(s.load_1m_ppm),
              static_cast<int>(s.threads));
        o.add(",\"pos\":[");
        bool first_pos = true;
        for (int i = 0; i < s.pos_n; ++i) {
            if (!acct_env_ok(s.pos[i].acct, env)) continue;
            if (!first_pos) o.add(",");
            first_pos = false;
            o.add("{\"acct\":%u,\"name\":%u,\"today\":%lld,\"yday\":%lld,\"avg_px\":%lld,\"realized\":%lld,"
                  "\"unrealized\":%lld}",
                  static_cast<unsigned>(s.pos[i].acct), static_cast<unsigned>(s.pos[i].name),
                  static_cast<long long>(s.pos[i].today), static_cast<long long>(s.pos[i].yday),
                  static_cast<long long>(s.pos[i].avg_px), static_cast<long long>(s.pos[i].realized),
                  static_cast<long long>(s.pos[i].unrealized));
        }
        o.add("],\"orders\":[");
        pitlane::FlowOrderRow rows[32]{};
        const int nr = flow_ ? flow_->recent_orders(rows, 32) : 0;
        for (int i = 0; i < nr; ++i) {
            if (i) o.add(",");
            o.add("{\"tag\":%llu,\"name\":%u,\"side\":%u,\"phase\":%u,\"qty\":%lld,\"filled\":%lld,\"px\":%lld,"
                  "\"ts\":%lld}",
                  static_cast<unsigned long long>(rows[i].tag), static_cast<unsigned>(rows[i].name),
                  static_cast<unsigned>(rows[i].side), static_cast<unsigned>(rows[i].phase),
                  static_cast<long long>(rows[i].qty), static_cast<long long>(rows[i].filled),
                  static_cast<long long>(rows[i].px), static_cast<long long>(rows[i].updated_us));
        }
        o.add("]");
        add_rms(o);
        o.add(",\"note\":\"");
        for (const char* p = s.note; *p; ++p) {
            if (*p == '"' || *p == '\\') o.add("\\%c", *p);
            else if (static_cast<unsigned char>(*p) >= 0x20) o.add("%c", *p);
        }
        o.add("\"}");
        json_len_ = o.n;
    }

    void render_events(int64_t since) noexcept {
        pitlane::OpsAuditRec recs[32]{};
        const int n = ch_.recent_audit(recs, 32);
        Out o{json_, kJsonCap, 0};
        o.add("{\"events\":[");
        bool first = true;
        for (int i = 0; i < n; ++i) {
            if (since >= 0 && static_cast<uint64_t>(since) >= recs[i].seq) continue;
            if (!first) o.add(",");
            first = false;
            if (recs[i].kind == 0xff) {
                o.add("{\"seq\":%llu,\"kind\":\"risk\",\"code\":%d,\"msg\":\"risk\",\"ts\":%lld}",
                      static_cast<unsigned long long>(recs[i].seq), static_cast<int>(recs[i].code),
                      static_cast<long long>(recs[i].stamp_us));
            } else {
                o.add("{\"seq\":%llu,\"kind\":\"%s\",\"code\":%d,\"msg\":\"%s\",\"ts\":%lld}",
                      static_cast<unsigned long long>(recs[i].seq), pitlane::ops_kind_name(recs[i].kind),
                      static_cast<int>(recs[i].code), pitlane::ops_code_name(recs[i].code),
                      static_cast<long long>(recs[i].stamp_us));
            }
        }
        o.add("],\"cursor\":%lld}", static_cast<long long>(ch_.audit_claimed()));
        json_len_ = o.n;
    }

    void render_result(const pitlane::OpsResult& r, bool pending) noexcept {
        Out o{json_, kJsonCap, 0};
        char msg[64];
        std::snprintf(msg, sizeof msg, "%s", r.msg);
        o.add("{\"code\":%d,\"msg\":\"%s\",\"seq\":%llu,\"id\":%llu,\"pending\":%u}", static_cast<int>(r.code), msg,
              static_cast<unsigned long long>(r.seq), static_cast<unsigned long long>(r.id), pending ? 1u : 0u);
        json_len_ = o.n;
    }

    void render_me(const pitlane::OpsSession& s) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"user\":\"%s\",\"role\":\"%s\",\"perms\":%u,\"mode\":\"%s\"}", s.user,
              pitlane::ops_role_name(s.role), static_cast<unsigned>(s.perms), pitlane::acc_env_name(s.mode));
        json_len_ = o.n;
    }

    void render_symbols() noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"symbols\":[");
        for (int i = 0; i < series_.count(); ++i) {
            if (i) o.add(",");
            o.add("{\"name\":%u,\"ticks\":%d}", static_cast<unsigned>(series_.name_at(i)),
                  series_.ring_at(i).size());
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_series(uint32_t name, int64_t tf_us, int limit) noexcept {
        if (tf_us <= 0) tf_us = 1000000;
        if (limit <= 0) limit = 1;
        if (limit > 300) limit = 300;
        pitlane::SeriesBar bars[300]{};
        pitlane::SeriesRing<2048>* ring = series_.find(name);
        const int n = ring ? ring->bars(tf_us, limit, bars) : 0;
        Out o{json_, kJsonCap, 0};
        o.add("{\"name\":%u,\"tf_us\":%lld,\"bars\":[", static_cast<unsigned>(name), static_cast<long long>(tf_us));
        for (int i = 0; i < n; ++i) {
            if (i) o.add(",");
            o.add("{\"t\":%lld,\"o\":%lld,\"h\":%lld,\"l\":%lld,\"c\":%lld,\"v\":%lld}",
                  static_cast<long long>(bars[i].ts_us), static_cast<long long>(bars[i].open),
                  static_cast<long long>(bars[i].high), static_cast<long long>(bars[i].low),
                  static_cast<long long>(bars[i].close), static_cast<long long>(bars[i].ticks));
        }
        o.add("]");
        if (ring && ring->size() > 0) {
            const pitlane::SeriesTick t = ring->at(ring->size() - 1);
            o.add(",\"last\":{\"ts\":%lld,\"bid\":%lld,\"ask\":%lld,\"px\":%lld}",
                  static_cast<long long>(t.ts_us), static_cast<long long>(t.bid), static_cast<long long>(t.ask),
                  static_cast<long long>(t.last));
        }
        o.add("}");
        json_len_ = o.n;
    }

    void render_ticks(uint32_t name, int limit) noexcept {
        if (limit <= 0) limit = 1;
        if (limit > 300) limit = 300;
        pitlane::SeriesTick buf[300]{};
        pitlane::SeriesRing<2048>* ring = series_.find(name);
        int n = 0;
        if (ring) {
            const int total = ring->size();
            const int from = total > limit ? total - limit : 0;
            for (int i = from; i < total; ++i) buf[n++] = ring->at(i);
        }
        render_tick_array(name, buf, n);
    }

    void render_history(uint32_t name, int64_t since, int64_t until, int limit) noexcept {
        if (limit <= 0) limit = 1;
        if (limit > 300) limit = 300;
        if (until <= 0) until = INT64_MAX;
        pitlane::SeriesTick buf[300]{};
        int n = 0;
        if (tape_path_[0]) {
            n = pitlane::series_read_tape(tape_path_, name, since, until, limit, buf);
        } else if (pitlane::SeriesRing<2048>* ring = series_.find(name)) {
            const int total = ring->size();
            const int from = total > limit ? total - limit : 0;
            for (int i = from; i < total && n < limit; ++i) buf[n++] = ring->at(i);
        }
        render_tick_array(name, buf, n);
    }

    void render_tick_array(uint32_t name, const pitlane::SeriesTick* buf, int n) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"name\":%u,\"ticks\":[", static_cast<unsigned>(name));
        for (int i = 0; i < n; ++i) {
            if (i) o.add(",");
            o.add("{\"t\":%lld,\"bid\":%lld,\"ask\":%lld,\"px\":%lld,\"bq\":%lld,\"aq\":%lld}",
                  static_cast<long long>(buf[i].ts_us), static_cast<long long>(buf[i].bid),
                  static_cast<long long>(buf[i].ask), static_cast<long long>(buf[i].last),
                  static_cast<long long>(buf[i].bid_qty), static_cast<long long>(buf[i].ask_qty));
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_gateways(uint8_t env) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"gateways\":[");
        bool first = true;
        for (int i = 0; i < gw_.count(); ++i) {
            const pitlane::GwSpec& s = gw_.spec(i);
            if (s.env_bit != env) continue;
            const pitlane::GwStatus& st = gw_.status(i);
            if (!first) o.add(",");
            first = false;
            o.add("{\"id\":\"%s\",\"kind\":\"%s\",\"adapter\":\"%s\",\"acct\":\"%s\",\"env\":\"%s\","
                  "\"state\":\"%s\",\"note\":\"%s\",\"auto\":%u,\"available\":%u,\"since\":%lld}",
                  s.id, pitlane::gw_kind_name(s.kind), s.adapter, s.acct, s.env, pitlane::gw_state_name(st.state),
                  st.note, s.auto_start ? 1u : 0u, s.available ? 1u : 0u, static_cast<long long>(st.since_us));
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_instruments() noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"instruments\":[");
        bool first = true;
        for (int i = 0; i < instr_.count(); ++i) {
            const pitlane::InstrSpec& s = instr_.spec(i);
            if (!first) o.add(",");
            first = false;
            o.add("{\"name\":%u,\"class\":\"%s\",\"ccy\":\"%s\",\"tick\":%lld,\"mult\":%lld,\"margin_ppm\":%lld,"
                  "\"fee_ppm\":%lld,\"min_fee\":%lld,\"stamp_ppm\":%lld,\"short\":%u}",
                  static_cast<unsigned>(s.name), pitlane::instr_class_name(s.klass), s.ccy,
                  static_cast<long long>(s.tick), static_cast<long long>(s.mult),
                  static_cast<long long>(s.margin_ppm), static_cast<long long>(s.fee_ppm),
                  static_cast<long long>(s.min_fee), static_cast<long long>(s.stamp_ppm),
                  s.allow_short ? 1u : 0u);
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_accounts(uint8_t env) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"accounts\":[");
        bool first = true;
        for (int i = 0; i < accs_.count(); ++i) {
            const pitlane::AccountSpec& a = accs_.spec(i);
            if (a.env != env) continue;
            if (!first) o.add(",");
            first = false;
            o.add("{\"num\":%u,\"id\":\"%s\",\"name\":\"%s\",\"class\":\"%s\",\"ccy\":\"%s\",\"cash\":%lld,"
                  "\"env\":\"%s\",\"gateways\":[",
                  static_cast<unsigned>(a.num), a.id, a.name, a.klass, a.ccy, static_cast<long long>(a.cash),
                  pitlane::acc_env_name(a.env));
            bool first_gw = true;
            for (int g = 0; g < gw_.count(); ++g) {
                if (gw_.spec(g).acct_num != a.num) continue;
                if (!first_gw) o.add(",");
                first_gw = false;
                o.add("\"%s\"", gw_.spec(g).id);
            }
            o.add("],\"td_running\":%d}", td_running(a.num));
        }
        o.add("]}");
        json_len_ = o.n;
    }

    int td_running(uint32_t acct_num) const noexcept {
        int n = 0;
        for (int i = 0; i < gw_.count(); ++i) {
            if (gw_.spec(i).acct_num == acct_num && gw_.spec(i).kind == pitlane::kGwTd &&
                gw_.status(i).state == pitlane::kGwRunning) {
                ++n;
            }
        }
        return n;
    }

    void render_alarms(int64_t since, uint8_t env) noexcept {
        pitlane::Alarm list[32]{};
        const int n = alarms_.recent(list, 32, since, env, true);
        Out o{json_, kJsonCap, 0};
        o.add("{\"alarms\":[");
        for (int i = 0; i < n; ++i) {
            if (i) o.add(",");
            o.add("{\"seq\":%lld,\"ts\":%lld,\"kind\":\"%s\",\"sev\":\"%s\",\"env\":\"%s\",\"text\":\"%s\"}",
                  static_cast<long long>(list[i].seq), static_cast<long long>(list[i].stamp_us),
                  pitlane::alarm_kind_name(list[i].kind), pitlane::alarm_sev_name(list[i].sev),
                  pitlane::acc_env_name(list[i].env), list[i].text);
        }
        o.add("],\"cursor\":%lld}", static_cast<long long>(alarms_.claimed()));
        json_len_ = o.n;
    }

    static const char* clr_move_name(uint8_t kind) noexcept {
        switch (kind) {
            case pitlane::kClrDeposit: return "deposit";
            case pitlane::kClrWithdraw: return "withdraw";
            case pitlane::kClrDividend: return "dividend";
            case pitlane::kClrFee: return "fee";
            case pitlane::kClrInterest: return "interest";
            case pitlane::kClrSubscribe: return "subscribe";
            case pitlane::kClrRedeem: return "redeem";
            default: return "other";
        }
    }

    static const char* ledger_tier_name(uint8_t tier) noexcept {
        switch (static_cast<pitlane::LedgerTier>(tier)) {
            case pitlane::LedgerTier::Product: return "product";
            case pitlane::LedgerTier::Broker: return "broker";
            case pitlane::LedgerTier::Trader: return "trader";
            case pitlane::LedgerTier::Strategy: return "strategy";
            case pitlane::LedgerTier::TraderMerge: return "trader_merge";
            case pitlane::LedgerTier::StrategyMerge: return "strategy_merge";
            case pitlane::LedgerTier::StrategyByProduct: return "strategy_product";
            default: return "unknown";
        }
    }

    static const char* ledger_dir_name(uint8_t dir) noexcept {
        return dir == static_cast<uint8_t>(pitlane::LedgerSide::Short) ? "short" : "long";
    }

    // Counter account and position snapshot. query may carry tier, acct, name
    // and direction (1 long, 2 short); a direction filter also hides assets.
    void render_ledger(const char* query) noexcept {
        pitlane::LedgerFilter f{};
        int64_t v = 0;
        if (query) {
            if (trading_ops::query_i64(query, "tier", v) && v > 0 && v < 8) f.tier = static_cast<uint8_t>(v);
            if (trading_ops::query_i64(query, "acct", v) && v > 0) f.acct = static_cast<uint32_t>(v);
            if (trading_ops::query_i64(query, "name", v) && v > 0) f.name = static_cast<uint32_t>(v);
            if (trading_ops::query_i64(query, "direction", v) && v > 0) f.direction = static_cast<uint8_t>(v);
        }
        pitlane::LedgerAsset assets[64]{};
        pitlane::LedgerPosition positions[64]{};
        int next = 0;
        bool last = true;
        const int an = f.direction ? 0 : ledger_.query_assets(f, 0, assets, 64, &next, &last);
        const int pn = ledger_.query_positions(f, 0, positions, 64, &next, &last);
        Out o{json_, kJsonCap, 0};
        o.add("{\"version\":%u,\"stamp_us\":%lld,\"assets\":[", static_cast<unsigned>(ledger_.version()),
              static_cast<long long>(ledger_.stamp_us()));
        for (int i = 0; i < an; ++i) {
            const pitlane::LedgerAsset& a = assets[i];
            if (i) o.add(",");
            o.add("{\"acct\":%u,\"tier\":\"%s\",\"total_cash\":%lld,\"available_cash\":%lld,"
                  "\"asset_balance\":%lld,\"static_equity\":%lld,\"dynamic_equity\":%lld,"
                  "\"market_value\":%lld,\"margin\":%lld,\"frozen_cash\":%lld,\"frozen_margin\":%lld,"
                  "\"accumulated_fee\":%lld,\"intraday_fee\":%lld,\"realized_pnl\":%lld,"
                  "\"unrealized_pnl\":%lld,\"position_pnl\":%lld,\"close_pnl\":%lld,"
                  "\"credit\":%lld,\"available_credit\":%lld,\"total_debt\":%lld,\"fetch_balance\":%lld,"
                  "\"assure_ratio\":%lld,\"stamp_us\":%lld}",
                  static_cast<unsigned>(a.acct), ledger_tier_name(a.tier),
                  static_cast<long long>(a.total_cash), static_cast<long long>(a.available_cash),
                  static_cast<long long>(a.asset_balance), static_cast<long long>(a.static_equity),
                  static_cast<long long>(a.dynamic_equity), static_cast<long long>(a.market_value),
                  static_cast<long long>(a.margin), static_cast<long long>(a.frozen_cash),
                  static_cast<long long>(a.frozen_margin), static_cast<long long>(a.accumulated_fee),
                  static_cast<long long>(a.intraday_fee), static_cast<long long>(a.realized_pnl),
                  static_cast<long long>(a.unrealized_pnl), static_cast<long long>(a.position_pnl),
                  static_cast<long long>(a.close_pnl), static_cast<long long>(a.credit),
                  static_cast<long long>(a.available_credit), static_cast<long long>(a.total_debt),
                  static_cast<long long>(a.fetch_balance), static_cast<long long>(a.assure_ratio),
                  static_cast<long long>(a.stamp_us));
        }
        o.add("],\"positions\":[");
        for (int i = 0; i < pn; ++i) {
            const pitlane::LedgerPosition& p = positions[i];
            if (i) o.add(",");
            o.add("{\"acct\":%u,\"tier\":\"%s\",\"name\":%u,\"product\":%u,\"direction\":\"%s\","
                  "\"total\":%lld,\"initial_yesterday\":%lld,\"left_yesterday\":%lld,\"sellable\":%lld,"
                  "\"frozen\":%lld,\"yesterday_buy\":%lld,\"today_buy\":%lld,\"yesterday_sell\":%lld,"
                  "\"today_sell\":%lld,\"last_px\":%lld,\"avg_open_px\":%lld,\"cost_px\":%lld,"
                  "\"settlement_px\":%lld,\"pre_settlement_px\":%lld,\"margin\":%lld,"
                  "\"position_pnl\":%lld,\"close_pnl\":%lld,\"realized_pnl\":%lld,"
                  "\"unrealized_pnl\":%lld,\"commission\":%lld,\"stamp_us\":%lld}",
                  static_cast<unsigned>(p.acct), ledger_tier_name(p.tier), static_cast<unsigned>(p.name),
                  static_cast<unsigned>(p.product), ledger_dir_name(p.direction),
                  static_cast<long long>(p.total), static_cast<long long>(p.initial_yesterday),
                  static_cast<long long>(p.left_yesterday), static_cast<long long>(p.sellable),
                  static_cast<long long>(p.frozen), static_cast<long long>(p.yesterday_buy),
                  static_cast<long long>(p.today_buy), static_cast<long long>(p.yesterday_sell),
                  static_cast<long long>(p.today_sell), static_cast<long long>(p.last_px),
                  static_cast<long long>(p.avg_open_px), static_cast<long long>(p.cost_px),
                  static_cast<long long>(p.settlement_px), static_cast<long long>(p.pre_settlement_px),
                  static_cast<long long>(p.margin), static_cast<long long>(p.position_pnl),
                  static_cast<long long>(p.close_pnl), static_cast<long long>(p.realized_pnl),
                  static_cast<long long>(p.unrealized_pnl), static_cast<long long>(p.commission),
                  static_cast<long long>(p.stamp_us));
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_clearing(uint8_t env) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"accounts\":[");
        bool first = true;
        for (int i = 0; i < accs_.count() && i < 8; ++i) {
            if (accs_.spec(i).env != env) continue;
            pitlane::ClearingReport rep{};
            if (!clear_board_[i].read(rep)) continue;
            if (!first) o.add(",");
            first = false;
            o.add("{\"acct\":%u,\"id\":\"%s\",\"stamp_us\":%lld,\"base_ccy\":\"%s\",\"realized\":%lld,"
                  "\"unrealized\":%lld,\"fees\":%lld,\"margin\":%lld,\"market_value\":%lld,\"recon_diff\":%d",
                  static_cast<unsigned>(rep.acct), accs_.spec(i).id, static_cast<long long>(rep.stamp_us),
                  accs_.spec(i).ccy, static_cast<long long>(rep.realized),
                  static_cast<long long>(rep.unrealized), static_cast<long long>(rep.fees),
                  static_cast<long long>(rep.margin), static_cast<long long>(rep.market_value),
                  rep.recon_diff_rows);
            o.add(",\"cash_in\":%lld,\"deposits\":%lld,\"withdrawals\":%lld,\"cash_balance\":%lld,"
                  "\"cash_flow\":%lld,\"equity\":%lld,\"equity_settle\":%lld,\"available\":%lld",
                  static_cast<long long>(rep.cash_in), static_cast<long long>(rep.deposits),
                  static_cast<long long>(rep.withdrawals), static_cast<long long>(rep.cash_balance),
                  static_cast<long long>(rep.cash_flow), static_cast<long long>(rep.equity),
                  static_cast<long long>(rep.equity_settle), static_cast<long long>(rep.available));
            o.add(",\"commission\":%lld,\"stamp\":%lld,\"trades\":%lld,\"turnover\":%lld,"
                  "\"day_realized\":%lld,\"day_fees\":%lld,\"day_trades\":%lld,\"day_turnover\":%lld,"
                  "\"settle_us\":%lld,\"day_seq\":%u,\"day_settled\":%lld",
                  static_cast<long long>(rep.commission), static_cast<long long>(rep.stamp),
                  static_cast<long long>(rep.trades), static_cast<long long>(rep.turnover),
                  static_cast<long long>(rep.day_realized), static_cast<long long>(rep.day_fees),
                  static_cast<long long>(rep.day_trades), static_cast<long long>(rep.day_turnover),
                  static_cast<long long>(rep.settle_us), static_cast<unsigned>(rep.day_seq),
                  static_cast<long long>(rep.day_settled));
            o.add(",\"realized_settle\":%lld,\"unrealized_settle\":%lld,\"pre_equity\":%lld,"
                  "\"today_pnl\":%lld,\"total_pnl\":%lld,\"shares\":%lld,\"unit_nav\":%lld,"
                  "\"peak_nav\":%lld,\"drawdown_ppm\":%lld,\"fx_missing\":%d",
                  static_cast<long long>(rep.realized_settle), static_cast<long long>(rep.unrealized_settle),
                  static_cast<long long>(rep.pre_equity), static_cast<long long>(rep.today_pnl),
                  static_cast<long long>(rep.total_pnl), static_cast<long long>(rep.shares),
                  static_cast<long long>(rep.unit_nav), static_cast<long long>(rep.peak_nav),
                  static_cast<long long>(rep.drawdown_ppm), rep.fx_missing);
            o.add(",\"income\":%lld,\"expense\":%lld,\"net_flow\":%lld,\"cumulative_nav\":%lld,"
                  "\"distributions_ppm\":%lld,\"shares_in\":%lld,\"shares_out\":%lld,"
                  "\"mgmt_fee\":%lld,\"custody_fee\":%lld,\"sales_fee\":%lld,\"fin_interest\":%lld",
                  static_cast<long long>(rep.income), static_cast<long long>(rep.expense),
                  static_cast<long long>(rep.net_flow), static_cast<long long>(rep.cumulative_nav),
                  static_cast<long long>(rep.distributions_ppm), static_cast<long long>(rep.shares_in),
                  static_cast<long long>(rep.shares_out), static_cast<long long>(rep.mgmt_fee),
                  static_cast<long long>(rep.custody_fee), static_cast<long long>(rep.sales_fee),
                  static_cast<long long>(rep.fin_interest));
            o.add(",\"days\":[");
            const int nd = rep.days < 4 ? rep.days : 4;
            for (int d = 0; d < nd; ++d) {
                if (d) o.add(",");
                const pitlane::ClrDayStat& ds = rep.day[d];
                o.add("{\"day\":%u,\"settle_us\":%lld,\"equity\":%lld,\"cash_balance\":%lld,"
                      "\"unit_nav\":%lld,\"realized\":%lld,\"settled\":%lld,\"fees\":%lld,"
                      "\"trades\":%lld,\"turnover\":%lld,\"deposits\":%lld,\"withdrawals\":%lld,"
                      "\"pnl\":%lld,\"income\":%lld,\"expense\":%lld,\"cum_nav\":%lld}",
                      static_cast<unsigned>(ds.day_seq), static_cast<long long>(ds.settle_us),
                      static_cast<long long>(ds.equity), static_cast<long long>(ds.cash_balance),
                      static_cast<long long>(ds.unit_nav), static_cast<long long>(ds.realized),
                      static_cast<long long>(ds.settled), static_cast<long long>(ds.fees),
                      static_cast<long long>(ds.trades), static_cast<long long>(ds.turnover),
                      static_cast<long long>(ds.deposits), static_cast<long long>(ds.withdrawals),
                      static_cast<long long>(ds.pnl), static_cast<long long>(ds.income),
                      static_cast<long long>(ds.expense), static_cast<long long>(ds.cum_nav));
            }
            o.add("],\"cash\":[");
            for (int m = 0; m < rep.moves; ++m) {
                if (m) o.add(",");
                const pitlane::ClrCashMove& mv = rep.move[m];
                o.add("{\"seq\":%lld,\"ts\":%lld,\"kind\":\"%s\",\"ccy\":\"%s\",\"amount\":%lld,\"base\":%lld}",
                      static_cast<long long>(mv.seq), static_cast<long long>(mv.stamp_us),
                      clr_move_name(mv.kind), accs_.spec(i).ccy,
                      static_cast<long long>(mv.amount), static_cast<long long>(mv.base_amount));
            }
            o.add("],\"rows\":[");
            for (int r = 0; r < rep.rows; ++r) {
                if (r) o.add(",");
                const pitlane::ClearingRow& row = rep.row[r];
                const pitlane::InstrSpec* ins = instr_.by_name(row.name);
                o.add("{\"name\":%u,\"ccy\":\"%s\",\"fx_ppm\":%lld,\"long\":%lld,\"short\":%lld,"
                      "\"long_today\":%lld,\"long_yday\":%lld,\"short_today\":%lld,\"short_yday\":%lld,"
                      "\"avg_long\":%lld,\"avg_short\":%lld,\"realized\":%lld,\"realized_settle\":%lld,"
                      "\"unrealized\":%lld,\"unrealized_settle\":%lld,\"fees\":%lld,\"margin\":%lld,"
                      "\"mv\":%lld,\"mark\":%lld,\"settle_mark\":%lld,\"commission\":%lld,\"stamp\":%lld,"
                      "\"trades\":%lld,\"turnover\":%lld}",
                      static_cast<unsigned>(row.name), ins ? ins->ccy : "", static_cast<long long>(row.fx_ppm),
                      static_cast<long long>(row.long_qty), static_cast<long long>(row.short_qty),
                      static_cast<long long>(row.long_today), static_cast<long long>(row.long_yday),
                      static_cast<long long>(row.short_today), static_cast<long long>(row.short_yday),
                      static_cast<long long>(row.avg_long), static_cast<long long>(row.avg_short),
                      static_cast<long long>(row.realized), static_cast<long long>(row.realized_settle),
                      static_cast<long long>(row.unrealized), static_cast<long long>(row.unrealized_settle),
                      static_cast<long long>(row.fees), static_cast<long long>(row.margin),
                      static_cast<long long>(row.market_value), static_cast<long long>(row.mark),
                      static_cast<long long>(row.settle_mark), static_cast<long long>(row.commission),
                      static_cast<long long>(row.stamp), static_cast<long long>(row.trades),
                      static_cast<long long>(row.turnover));
            }
            o.add("],\"recon\":[");
            for (int r = 0; r < rep.recon_rows; ++r) {
                if (r) o.add(",");
                o.add("{\"name\":%u,\"clr\":%lld,\"pms\":%lld,\"diff\":%lld}",
                      static_cast<unsigned>(rep.recon[r].name), static_cast<long long>(rep.recon[r].clr_qty),
                      static_cast<long long>(rep.recon[r].pms_qty), static_cast<long long>(rep.recon[r].diff));
            }
            o.add("]}");
        }
        o.add("]}");
        json_len_ = o.n;
    }

    // Day records for the clearing tab, newest last, capped by `days`.
    void render_clear_history(uint8_t env, uint32_t acct, int days) noexcept {
        if (days <= 0) days = 8;
        if (days > pitlane::kClrMaxDays) days = pitlane::kClrMaxDays;
        Out o{json_, kJsonCap, 0};
        o.add("{\"accounts\":[");
        bool first = true;
        for (int i = 0; i < accs_.count() && i < 8; ++i) {
            const pitlane::AccountSpec& a = accs_.spec(i);
            if (a.env != env) continue;
            if (acct && a.num != acct) continue;
            pitlane::ClearingReport rep{};
            if (!clear_board_[i].read(rep)) continue;
            if (!first) o.add(",");
            first = false;
            const int total = rep.days < days ? rep.days : days;
            o.add("{\"acct\":%u,\"id\":\"%s\",\"days\":[", static_cast<unsigned>(a.num), a.id);
            for (int d = rep.days - total; d < rep.days; ++d) {
                if (d != rep.days - total) o.add(",");
                const pitlane::ClrDayStat& ds = rep.day[d];
                o.add("{\"day\":%u,\"settle_us\":%lld,\"equity\":%lld,\"unit_nav\":%lld,"
                      "\"cum_nav\":%lld,\"pnl\":%lld,\"realized\":%lld,\"settled\":%lld,"
                      "\"fees\":%lld,\"income\":%lld,\"expense\":%lld,\"trades\":%lld,"
                      "\"turnover\":%lld,\"deposits\":%lld,\"withdrawals\":%lld}",
                      static_cast<unsigned>(ds.day_seq), static_cast<long long>(ds.settle_us),
                      static_cast<long long>(ds.equity), static_cast<long long>(ds.unit_nav),
                      static_cast<long long>(ds.cum_nav), static_cast<long long>(ds.pnl),
                      static_cast<long long>(ds.realized), static_cast<long long>(ds.settled),
                      static_cast<long long>(ds.fees), static_cast<long long>(ds.income),
                      static_cast<long long>(ds.expense), static_cast<long long>(ds.trades),
                      static_cast<long long>(ds.turnover), static_cast<long long>(ds.deposits),
                      static_cast<long long>(ds.withdrawals));
            }
            o.add("]}");
        }
        o.add("]}");
        json_len_ = o.n;
    }

    // Period sums: today, the last seven days, the last thirty, and YTD.
    void render_clear_period(uint8_t env, uint32_t acct) noexcept {
        const int64_t now = now_us();
        Out o{json_, kJsonCap, 0};
        o.add("{\"accounts\":[");
        bool first = true;
        for (int i = 0; i < accs_.count() && i < 8; ++i) {
            const pitlane::AccountSpec& a = accs_.spec(i);
            if (a.env != env) continue;
            if (acct && a.num != acct) continue;
            pitlane::ClearingReport rep{};
            if (!clear_board_[i].read(rep)) continue;
            const pitlane::ClrPeriod p = pitlane::clr_period(rep, now);
            if (!first) o.add(",");
            first = false;
            o.add("{\"acct\":%u,\"id\":\"%s\",\"today\":%lld,\"d7\":%lld,\"d30\":%lld,\"ytd\":%lld}",
                  static_cast<unsigned>(a.num), a.id, static_cast<long long>(p.today),
                  static_cast<long long>(p.d7), static_cast<long long>(p.d30), static_cast<long long>(p.ytd));
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_recon(const pitlane::ReconOut& r, const char* path) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"path\":\"%s\",\"count\":%d,\"qty_breaks\":%d,\"money_breaks\":%d,\"rows\":[",
              path ? path : "", r.count, r.qty_breaks, r.money_breaks);
        for (int i = 0; i < r.count; ++i) {
            if (i) o.add(",");
            const pitlane::ReconLine& l = r.line[i];
            o.add("{\"name\":%u,\"present\":%u,\"qty\":[%lld,%lld,%lld],\"equity\":[%lld,%lld,%lld],"
                  "\"dqty\":[%lld,%lld,%lld],\"deq\":[%lld,%lld,%lld],\"qty_bad\":%u,\"money_bad\":%u}",
                  static_cast<unsigned>(l.name), static_cast<unsigned>(l.present),
                  static_cast<long long>(l.qty[0]), static_cast<long long>(l.qty[1]),
                  static_cast<long long>(l.qty[2]), static_cast<long long>(l.equity[0]),
                  static_cast<long long>(l.equity[1]), static_cast<long long>(l.equity[2]),
                  static_cast<long long>(l.dqty[0]), static_cast<long long>(l.dqty[1]),
                  static_cast<long long>(l.dqty[2]), static_cast<long long>(l.dequity[0]),
                  static_cast<long long>(l.dequity[1]), static_cast<long long>(l.dequity[2]),
                  static_cast<unsigned>(l.qty_bad), static_cast<unsigned>(l.money_bad));
        }
        o.add("]}");
        json_len_ = o.n;
    }

    // A sealed read can lose the race with the beat thread; retry briefly so a
    // single collision does not surface as an empty risk page.
    bool read_snapshot(pitlane::StatusSnapshot& s) noexcept {
        for (int i = 0; i < 3; ++i) {
            if (board_.read(s)) return true;
            std::this_thread::yield();
        }
        return false;
    }

    // Host health is re-evaluated on read so a stalled trading thread shows up
    // as heartbeat lag rather than a frozen snapshot.
    bool render_host_health(pitlane::HttpResp& resp) noexcept {
        pitlane::StatusSnapshot s{};
        const int64_t now = now_us();
        if (!read_snapshot(s)) {
            static const char kBusy[] = "{\"code\":4,\"msg\":\"busy\"}";
            resp.json(503, kBusy, sizeof kBusy - 1);
            return true;
        }
        pitlane::HealthSample hs{};
        hs.cpu_ppm = s.cpu_ppm;
        hs.mem_ppm = s.mem_ppm;
        hs.load_ppm = s.load_1m_ppm;
        hs.threads = s.threads;
        hs.fds = s.fds;
        hs.disk_used_ppm = s.disk_used_ppm;
        hs.rx_bps = s.rx_bps;
        hs.tx_bps = s.tx_bps;
        hs.uptime_s = s.host_uptime_s;
        hs.heartbeat_us = s.last_beat_us > 0 && now > s.last_beat_us ? now - s.last_beat_us : 0;
        pitlane::HealthState st{};
        pitlane::health_eval(hs, health_limits_, st);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"stamp_us\":%lld,\"level\":%u,\"bits\":%u,\"cpu_ppm\":%d,\"mem_ppm\":%d,"
              "\"load_ppm\":%d,\"threads\":%d,\"fds\":%d,\"disk_used_ppm\":%d,\"rx_bps\":%lld,\"tx_bps\":%lld,"
              "\"uptime_s\":%lld,\"heartbeat_us\":%lld,\"limits\":{\"cpu_ppm\":%d,\"mem_ppm\":%d,"
              "\"disk_ppm\":%d,\"fds\":%d,\"heartbeat_us\":%lld}}",
              static_cast<long long>(now), static_cast<unsigned>(st.level), static_cast<unsigned>(st.bits),
              hs.cpu_ppm, hs.mem_ppm, hs.load_ppm, hs.threads, hs.fds, hs.disk_used_ppm,
              static_cast<long long>(hs.rx_bps), static_cast<long long>(hs.tx_bps),
              static_cast<long long>(hs.uptime_s), static_cast<long long>(hs.heartbeat_us), health_limits_.cpu_ppm,
              health_limits_.mem_ppm, health_limits_.disk_ppm, health_limits_.fds,
              static_cast<long long>(health_limits_.heartbeat_us));
        resp.json(200, json_, static_cast<uint32_t>(o.n));
        return true;
    }

    // Trade blotter: what the clearing book actually booked, per account.
    void render_clear_trades(uint8_t env, uint32_t acct, int64_t since) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"trades\":[");
        bool first = true;
        for (int i = 0; i < accs_.count() && i < 8; ++i) {
            const pitlane::AccountSpec& a = accs_.spec(i);
            if (a.env != env) continue;
            if (acct && a.num != acct) continue;
            pitlane::ClrTrade list[64]{};
            const int n = books_[i].trades_since(since, list, 64);
            for (int k = 0; k < n; ++k) {
                if (!first) o.add(",");
                first = false;
                o.add("{\"seq\":%lld,\"ts\":%lld,\"acct\":%u,\"id\":\"%s\",\"name\":%u,\"side\":%u,"
                      "\"qty\":%lld,\"px\":%lld,\"fee\":%lld,\"realized\":%lld}",
                      static_cast<long long>(list[k].seq), static_cast<long long>(list[k].stamp_us),
                      static_cast<unsigned>(a.num), a.id, static_cast<unsigned>(list[k].name),
                      static_cast<unsigned>(list[k].side), static_cast<long long>(list[k].qty),
                      static_cast<long long>(list[k].px), static_cast<long long>(list[k].fee),
                      static_cast<long long>(list[k].realized));
            }
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_users() noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"users\":[");
        for (int i = 0; i < users_.count(); ++i) {
            if (i) o.add(",");
            o.add("{\"name\":\"%s\",\"role\":\"%s\",\"enabled\":%u}", users_.at(i).name,
                  pitlane::ops_role_name(users_.at(i).role), users_.at(i).enabled ? 1u : 0u);
        }
        o.add("]}");
        json_len_ = o.n;
    }

    bool acct_env_ok(uint32_t num, uint8_t env) const noexcept {
        const int i = accs_.index_of_num(num);
        return i >= 0 && accs_.spec(i).env == env;
    }

    static uint8_t sess_env(const pitlane::OpsSession& s) noexcept {
        return s.mode ? pitlane::kEnvLive : pitlane::kEnvTest;
    }

    bool mode_denied(pitlane::HttpResp& resp, const char* what) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":8,\"msg\":\"mode mismatch: %s\"}", what ? what : "target");
        resp.json(409, json_, static_cast<uint32_t>(o.n));
        return true;
    }

    // Resolves the funding account for an order request. Missing -> first
    // account of the session's mode; a cross-mode account is rejected.
    uint32_t order_acct(const pitlane::OpsSession& s, const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        const uint8_t env = sess_env(s);
        char tok[24]{};
        uint32_t a = 0;
        if (trading_ops::key_str(req.body, req.body_len, "acct", tok, sizeof tok)) a = accs_.resolve(tok);
        if (a == 0) {
            for (int i = 0; i < accs_.count(); ++i) {
                if (accs_.spec(i).env == env) {
                    a = accs_.spec(i).num;
                    break;
                }
            }
        }
        if (a == 0 || !acct_env_ok(a, env)) {
            mode_denied(resp, "account not in this mode");
            return 0;
        }
        return a;
    }

    // Resolves the account named in a book request, defaulting to the first
    // account of the session mode. Zero means the request does not fit.
    uint32_t body_acct(const pitlane::HttpReq& req, uint8_t env) noexcept {
        char tok[24]{};
        uint32_t num = 0;
        if (trading_ops::key_str(req.body, req.body_len, "acct", tok, sizeof tok)) num = accs_.resolve(tok);
        if (num == 0) {
            for (int i = 0; i < accs_.count(); ++i) {
                if (accs_.spec(i).env == env) {
                    num = accs_.spec(i).num;
                    break;
                }
            }
        }
        if (num == 0 || !acct_env_ok(num, env)) return 0;
        return num;
    }

    bool write_clear_csv(uint8_t env, char* out, int cap) noexcept {
        char dir[512];
        trading_ops::conf_sibling(conf_path_, "", dir, sizeof dir);
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        char path[640];
        std::snprintf(path, sizeof path, "%sclear-%04d%02d%02d-%s.csv", dir, tm.tm_year + 1900, tm.tm_mon + 1,
                      tm.tm_mday, pitlane::acc_env_name(env));
        std::FILE* f = std::fopen(path, "w");
        if (!f) return false;
        std::fprintf(f,
                     "mode,acct,id,row,name,ccy,long,short,long_today,long_yday,short_today,short_yday,"
                     "avg_long,avg_short,realized,realized_settle,unrealized,unrealized_settle,fees,margin,mv,"
                     "commission,stamp,trades,turnover,equity,equity_settle,available,cash_balance,today_pnl,"
                     "unit_nav\n");
        for (int i = 0; i < accs_.count() && i < 8; ++i) {
            if (accs_.spec(i).env != env) continue;
            pitlane::ClearingReport rep{};
            if (!clear_board_[i].read(rep)) continue;
            std::fprintf(f,
                         "%u,%u,%s,TOTAL,0,%s,%lld,%lld,0,0,0,0,0,0,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
                         "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
                         static_cast<unsigned>(env), static_cast<unsigned>(rep.acct), accs_.spec(i).id,
                         accs_.spec(i).ccy, static_cast<long long>(rep.sum_long()),
                         static_cast<long long>(rep.sum_short()), static_cast<long long>(rep.realized),
                         static_cast<long long>(rep.realized_settle), static_cast<long long>(rep.unrealized),
                         static_cast<long long>(rep.unrealized_settle), static_cast<long long>(rep.fees),
                         static_cast<long long>(rep.margin), static_cast<long long>(rep.market_value),
                         static_cast<long long>(rep.commission), static_cast<long long>(rep.stamp),
                         static_cast<long long>(rep.trades), static_cast<long long>(rep.turnover),
                         static_cast<long long>(rep.equity), static_cast<long long>(rep.equity_settle),
                         static_cast<long long>(rep.available), static_cast<long long>(rep.cash_balance),
                         static_cast<long long>(rep.today_pnl), static_cast<long long>(rep.unit_nav));
            for (int r = 0; r < rep.rows; ++r) {
                const pitlane::ClearingRow& row = rep.row[r];
                const pitlane::InstrSpec* ins = instr_.by_name(row.name);
                const int64_t pnl = row.realized + row.unrealized - row.fees;
                std::fprintf(f,
                             "%u,%u,%s,NAME,%u,%s,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
                             "%lld,%lld,%lld,%lld,%lld,%lld,%lld,0,0,0,0,%lld,0\n",
                             static_cast<unsigned>(env), static_cast<unsigned>(rep.acct), accs_.spec(i).id,
                             static_cast<unsigned>(row.name), ins ? ins->ccy : "",
                             static_cast<long long>(row.long_qty), static_cast<long long>(row.short_qty),
                             static_cast<long long>(row.long_today), static_cast<long long>(row.long_yday),
                             static_cast<long long>(row.short_today), static_cast<long long>(row.short_yday),
                             static_cast<long long>(row.avg_long), static_cast<long long>(row.avg_short),
                             static_cast<long long>(row.realized), static_cast<long long>(row.realized_settle),
                             static_cast<long long>(row.unrealized), static_cast<long long>(row.unrealized_settle),
                             static_cast<long long>(row.fees), static_cast<long long>(row.margin),
                             static_cast<long long>(row.market_value), static_cast<long long>(row.commission),
                             static_cast<long long>(row.stamp), static_cast<long long>(row.trades),
                             static_cast<long long>(row.turnover), static_cast<long long>(pnl));
            }
        }
        std::fclose(f);
        std::snprintf(out, static_cast<std::size_t>(cap), "%s", path);
        return true;
    }

    // ---- 行情五档、逐笔与逐跳延迟 ---------------------------------------

    void render_market() noexcept {
        pitlane::MarketSnap s{};
        const bool ok = market_live_ && market_live_->read(s);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"available\":%u,\"live_seq\":%llu,\"stamp_us\":%lld,\"decoded\":%llu,"
              "\"decode_err\":%llu,\"prints\":%llu,\"books\":%u,\"stale\":%llu,\"crossed\":%llu,"
              "\"rebuilds\":%llu,\"fan_sinks\":%u",
              ok ? 1u : 0u, static_cast<unsigned long long>(s.seq), static_cast<long long>(s.stamp_us),
              static_cast<unsigned long long>(s.decoded), static_cast<unsigned long long>(s.decode_err),
              static_cast<unsigned long long>(s.prints), static_cast<unsigned>(s.books),
              static_cast<unsigned long long>(s.stale), static_cast<unsigned long long>(s.crossed),
              static_cast<unsigned long long>(s.rebuilds), static_cast<unsigned>(s.fan_sinks));
        o.add(",\"book\":[");
        bool first = true;
        if (ok) {
            for (int i = 0; i < pitlane::kMarketBooks; ++i) {
                const pitlane::MarketBook& b = s.book[i];
                if (b.name == 0) continue;
                if (!first) o.add(",");
                first = false;
                o.add("{\"name\":%u,\"status\":%u,\"event\":%u,\"levels\":%u,\"exch_us\":%lld,"
                      "\"last\":%lld,\"upper\":%lld,\"lower\":%lld,\"imbalance_ppm\":%d,"
                      "\"snaps\":%llu,\"updates\":%llu,\"rebuilds\":%llu,\"stale\":%llu,\"crossed\":%llu",
                      static_cast<unsigned>(b.name), static_cast<unsigned>(b.status),
                      static_cast<unsigned>(b.event), static_cast<unsigned>(b.levels),
                      static_cast<long long>(b.exch_us), static_cast<long long>(b.last_px),
                      static_cast<long long>(b.upper_limit), static_cast<long long>(b.lower_limit),
                      static_cast<int>(b.imbalance_ppm), static_cast<unsigned long long>(b.snaps),
                      static_cast<unsigned long long>(b.updates), static_cast<unsigned long long>(b.rebuilds),
                      static_cast<unsigned long long>(b.stale), static_cast<unsigned long long>(b.crossed));
                o.add(",\"bid\":[");
                for (int k = 0; k < pitlane::kMarketDepth; ++k) {
                    if (k) o.add(",");
                    o.add("[%lld,%lld]", static_cast<long long>(b.bid[k].px),
                          static_cast<long long>(b.bid[k].qty));
                }
                o.add("],\"ask\":[");
                for (int k = 0; k < pitlane::kMarketDepth; ++k) {
                    if (k) o.add(",");
                    o.add("[%lld,%lld]", static_cast<long long>(b.ask[k].px),
                          static_cast<long long>(b.ask[k].qty));
                }
                o.add("]}");
            }
        }
        o.add("],\"ticks\":[");
        if (ok) {
            for (uint32_t i = 0; i < s.print_n; ++i) {
                const pitlane::MarketPrint& p = s.print[i];
                if (i) o.add(",");
                o.add("{\"name\":%u,\"aggressor\":%u,\"exch_us\":%lld,\"px\":%lld,\"qty\":%lld,\"volume\":%lld}",
                      static_cast<unsigned>(p.name), static_cast<unsigned>(p.aggressor),
                      static_cast<long long>(p.exch_us), static_cast<long long>(p.px),
                      static_cast<long long>(p.qty), static_cast<long long>(p.volume));
            }
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_hops() noexcept {
        static const pitlane::HopLedger kEmpty{};
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"scale_on\":%u,", scale_ok_ ? 1u : 0u);
        hop_json(o, "md", md_hops_ ? *md_hops_ : kEmpty);
        o.add(",");
        hop_json(o, "ops", hops_);
        o.add("}");
        json_len_ = o.n;
    }

    // 当前画像与基线对比。正 delta 表示比基线慢，超过 tolerance 记为回归。
    void render_baseline(int64_t tolerance_ppm) noexcept {
        pitlane::PerfBaseline cur;
        collect_perf(cur);
        pitlane::PerfDiff diff[pitlane::kPerfRows]{};
        const int bad = base_.compare(cur, tolerance_ppm, diff, pitlane::kPerfRows);
        int written = base_.count();
        for (int i = 0; i < cur.count(); ++i) {
            if (base_.find(cur.at(i).name) < 0) ++written;
        }
        if (written > pitlane::kPerfRows) written = pitlane::kPerfRows;
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"rev\":\"%s\",\"base_rows\":%d,\"cur_rows\":%d,\"tolerance_ppm\":%lld,"
              "\"regressed\":%d,\"diffs\":[",
              baseline_rev_, base_.count(), cur.count(), static_cast<long long>(tolerance_ppm), bad);
        for (int i = 0; i < written; ++i) {
            if (i) o.add(",");
            o.add("{\"name\":\"%s\",\"base_p99\":%llu,\"cur_p99\":%llu,\"delta_ppm\":%lld,"
                  "\"regressed\":%u,\"missing\":%u}",
                  diff[i].name, static_cast<unsigned long long>(diff[i].base_p99),
                  static_cast<unsigned long long>(diff[i].cur_p99), static_cast<long long>(diff[i].delta_ppm),
                  static_cast<unsigned>(diff[i].regressed), static_cast<unsigned>(diff[i].missing));
        }
        o.add("],\"current\":[");
        for (int i = 0; i < cur.count(); ++i) {
            const pitlane::PerfLine& l = cur.at(i);
            if (i) o.add(",");
            o.add("{\"name\":\"%s\",\"n\":%llu,\"p50\":%llu,\"p99\":%llu,\"max\":%llu}", l.name,
                  static_cast<unsigned long long>(l.n), static_cast<unsigned long long>(l.p50),
                  static_cast<unsigned long long>(l.p99), static_cast<unsigned long long>(l.max));
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_flame() noexcept {
        char folded[4096];
        const int n = flame_.folded(folded, sizeof folded);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"nodes\":%d,\"depth\":%d,\"folded\":\"", flame_.nodes(), flame_.depth());
        if (n > 0) json_escape(o, folded, n);
        o.add("\"}");
        json_len_ = o.n;
    }

    // ---- 席位、日志、灰度、回放 -----------------------------------------

    void render_seats() noexcept {
        seats_.tick_window();
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"n\":%d,\"seats\":[", seats_.count());
        for (int i = 0; i < seats_.count(); ++i) {
            const pitlane::SeatRow& s = seats_.at(i);
            if (i) o.add(",");
            o.add("{\"id\":%u,\"name\":\"%s\",\"broker\":\"%s\",\"exchange\":\"%s\",\"channel\":\"%s\","
                  "\"state\":\"%s\",\"enabled\":%u,\"orders\":%llu,\"rejects\":%llu,\"reconnects\":%llu,"
                  "\"window\":%d,\"last_us\":%lld}",
                  static_cast<unsigned>(s.spec.id), s.spec.name, s.spec.broker, s.spec.exchange,
                  s.spec.channel, pitlane::seat_state_name(s.state), static_cast<unsigned>(s.spec.enabled),
                  static_cast<unsigned long long>(s.orders), static_cast<unsigned long long>(s.rejects),
                  static_cast<unsigned long long>(s.reconnects), static_cast<int>(s.window_orders),
                  static_cast<long long>(s.last_us));
        }
        o.add("],\"channels\":[");
        for (int c = 0; c < seats_.channels(); ++c) {
            if (c) o.add(",");
            o.add("{\"name\":\"%s\",\"seats\":%d}", seats_.channel_name(c), seats_.channel_seats(c));
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_logs(const char* q, int64_t since_us, uint8_t min_level, uint8_t kind, int limit) noexcept {
        log_alarms();
        if (limit <= 0) limit = 50;
        if (limit > 200) limit = 200;
        static pitlane::LogLine rows[200];
        const int n = logs_.search(q, since_us, min_level, kind, rows, limit);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"n\":%d,\"lines\":[", n);
        for (int i = 0; i < n; ++i) {
            const pitlane::LogLine& l = rows[i];
            if (i) o.add(",");
            o.add("{\"seq\":%llu,\"ts_us\":%lld,\"level\":\"%s\",\"kind\":\"%s\",\"text\":\"",
                  static_cast<unsigned long long>(l.seq), static_cast<long long>(l.ts_us),
                  pitlane::log_level_name(l.level), pitlane::log_kind_name(l.kind));
            json_escape(o, l.text, l.len);
            o.add("\"}");
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_canary() noexcept {
        const pitlane::CanaryReport r = canary_.report();
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"state\":\"%s\",\"gate_ok\":%u,\"routed\":%llu,\"ok\":%llu,\"rejects\":%llu,"
              "\"reject_ppm\":%d,\"avg_slip\":%lld,\"plan\":{\"id\":%u,\"version\":\"%s\",\"pct_ppm\":%d,"
              "\"min_samples\":%d,\"max_reject_ppm\":%d,\"max_slip\":%lld}}",
              pitlane::canary_state_name(r.state), static_cast<unsigned>(r.gate_ok),
              static_cast<unsigned long long>(r.routed), static_cast<unsigned long long>(r.ok),
              static_cast<unsigned long long>(r.rejects), static_cast<int>(r.reject_ppm),
              static_cast<long long>(r.avg_slip), static_cast<unsigned>(canary_plan_.id),
              canary_plan_.version, static_cast<int>(canary_plan_.pct_ppm),
              static_cast<int>(canary_plan_.min_samples), static_cast<int>(canary_plan_.max_reject_ppm),
              static_cast<long long>(canary_plan_.max_slip));
        json_len_ = o.n;
    }

    // 回放游标指向的行情。没有录制的序列时返回空数组。
    void render_replay(int64_t at_us, int limit) noexcept {
        int64_t cursor = replay_.report().cursor_us;
        (void)replay_.advance(now_us(), cursor);
        if (at_us <= 0) at_us = cursor;
        const pitlane::ReplayReport r = replay_.report();
        if (limit <= 0) limit = 20;
        if (limit > 100) limit = 100;
        pitlane::SeriesRing<2048>* ring = series_.find(1);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"state\":\"%s\",\"from_us\":%lld,\"to_us\":%lld,\"cursor_us\":%lld,"
              "\"speed_ppm\":%lld,\"progress_ppm\":%d,\"frames\":%llu,\"at_us\":%lld,\"ticks\":[",
              pitlane::replay_state_name(r.state), static_cast<long long>(r.from_us),
              static_cast<long long>(r.to_us), static_cast<long long>(r.cursor_us),
              static_cast<long long>(r.speed_ppm), static_cast<int>(r.progress_ppm),
              static_cast<unsigned long long>(r.frames), static_cast<long long>(at_us));
        int n = 0;
        if (ring) {
            const int total = ring->size();
            int start = 0;
            while (start < total && ring->at(start).ts_us < at_us) ++start;
            for (int i = start; i < total && n < limit; ++i, ++n) {
                const pitlane::SeriesTick t = ring->at(i);
                if (n) o.add(",");
                o.add("{\"ts_us\":%lld,\"bid\":%lld,\"ask\":%lld,\"px\":%lld}", static_cast<long long>(t.ts_us),
                      static_cast<long long>(t.bid), static_cast<long long>(t.ask),
                      static_cast<long long>(t.last));
            }
        }
        o.add("]}");
        json_len_ = o.n;
    }

    bool post_seat(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        char action[12]{};
        int64_t id = 0;
        (void)trading_ops::query_str(req.query, "action", action, sizeof action);
        (void)trading_ops::query_i64(req.query, "id", id);
        bool ok = false;
        if (std::strcmp(action, "start") == 0) ok = seats_.start(static_cast<uint32_t>(id), now_us());
        else if (std::strcmp(action, "stop") == 0) ok = seats_.stop(static_cast<uint32_t>(id));
        else if (std::strcmp(action, "fault") == 0) ok = seats_.fault(static_cast<uint32_t>(id), now_us());
        char line[96];
        std::snprintf(line, sizeof line, "seat %lld %s %s", static_cast<long long>(id), action,
                      ok ? "ok" : "rejected");
        logs_.add(now_us(), ok ? pitlane::kLogInfo : pitlane::kLogWarn, pitlane::kLogOps, line);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":%d}", ok ? 0 : 1);
        resp.json(ok ? 200 : 409, json_, static_cast<uint32_t>(o.n));
        return true;
    }

    bool post_canary(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        char action[12]{};
        char version[16]{};
        (void)trading_ops::query_str(req.query, "action", action, sizeof action);
        (void)trading_ops::query_str(req.query, "version", version, sizeof version);
        int64_t id = 1;
        int64_t pct = 100000;
        int64_t min_samples = 100;
        int64_t max_reject = 50000;
        int64_t max_slip = 1;
        (void)trading_ops::query_i64(req.query, "id", id);
        (void)trading_ops::query_i64(req.query, "pct_ppm", pct);
        (void)trading_ops::query_i64(req.query, "min_samples", min_samples);
        (void)trading_ops::query_i64(req.query, "max_reject_ppm", max_reject);
        (void)trading_ops::query_i64(req.query, "max_slip", max_slip);
        bool ok = false;
        if (std::strcmp(action, "plan") == 0) {
            pitlane::CanaryPlan p{};
            p.id = static_cast<uint32_t>(id);
            std::snprintf(p.version, sizeof p.version, "%s", version[0] ? version : "v1");
            p.pct_ppm = static_cast<int32_t>(pct);
            p.min_samples = static_cast<int32_t>(min_samples);
            p.max_reject_ppm = static_cast<int32_t>(max_reject);
            p.max_slip = max_slip;
            ok = canary_.plan(p);
            if (ok) canary_plan_ = canary_.config();
        } else if (std::strcmp(action, "promote") == 0) {
            ok = canary_.promote();
        } else if (std::strcmp(action, "rollback") == 0) {
            ok = canary_.rollback();
        }
        char line[112];
        std::snprintf(line, sizeof line, "canary %s id=%lld %s", action, static_cast<long long>(id),
                      ok ? "ok" : "rejected");
        logs_.add(now_us(), ok ? pitlane::kLogInfo : pitlane::kLogWarn, pitlane::kLogOps, line);
        const pitlane::CanaryReport r = canary_.report();
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":%d,\"state\":\"%s\",\"gate_ok\":%u}", ok ? 0 : 1,
              pitlane::canary_state_name(r.state), static_cast<unsigned>(r.gate_ok));
        resp.json(ok ? 200 : 409, json_, static_cast<uint32_t>(o.n));
        return true;
    }

    // 回放控制只由 HTTP 线程改写；游标推进也在读取时完成，交易线程完全不碰。
    bool post_replay(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        char action[12]{};
        (void)trading_ops::query_str(req.query, "action", action, sizeof action);
        int64_t from = 0;
        int64_t to = 0;
        int64_t at = 0;
        int64_t speed = 1000000;
        int64_t frames = 1;
        (void)trading_ops::query_i64(req.query, "from_us", from);
        (void)trading_ops::query_i64(req.query, "to_us", to);
        (void)trading_ops::query_i64(req.query, "at_us", at);
        (void)trading_ops::query_i64(req.query, "speed_ppm", speed);
        (void)trading_ops::query_i64(req.query, "delta_us", frames);
        const int64_t now = now_us();
        bool ok = false;
        if (std::strcmp(action, "load") == 0) {
            pitlane::SeriesRing<2048>* ring = series_.find(1);
            if (ring && ring->size() > 0) {
                if (from == 0) from = ring->at(0).ts_us;
                if (to == 0) to = ring->at(ring->size() - 1).ts_us;
            }
            if (to <= from) to = from + 1;
            replay_.load(from, to, speed);
            ok = true;
        } else if (std::strcmp(action, "play") == 0) {
            ok = replay_.play(now);
        } else if (std::strcmp(action, "pause") == 0) {
            ok = replay_.pause();
        } else if (std::strcmp(action, "stop") == 0) {
            ok = replay_.stop();
        } else if (std::strcmp(action, "seek") == 0) {
            ok = replay_.seek(at, now);
        } else if (std::strcmp(action, "step") == 0) {
            ok = replay_.step(frames);
        } else if (std::strcmp(action, "speed") == 0) {
            ok = replay_.set_speed(speed);
        }
        char line[112];
        std::snprintf(line, sizeof line, "replay %s %s", action, ok ? "ok" : "rejected");
        logs_.add(now, ok ? pitlane::kLogInfo : pitlane::kLogWarn, pitlane::kLogMarket, line);
        const pitlane::ReplayReport r = replay_.report();
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":%d,\"state\":\"%s\",\"cursor_us\":%lld,\"progress_ppm\":%d}", ok ? 0 : 1,
              pitlane::replay_state_name(r.state), static_cast<long long>(r.cursor_us),
              static_cast<int>(r.progress_ppm));
        resp.json(ok ? 200 : 409, json_, static_cast<uint32_t>(o.n));
        return true;
    }

    // Submits one command and waits briefly for the control plane. Returns
    // false when the channel is full; the caller renders the response.
    bool send(const pitlane::OpsCommand& cmd, pitlane::OpsResult& r, bool& pending) noexcept {
        pitlane::OpsCommand c = cmd;
        c.seq = ch_.alloc_seq();
        c.id = c.id ? c.id : c.seq;
        const bool canary = canary_.pick(c.seq);
        const int seat = pick_seat(now_us());
        if (!ch_.submit(c)) return false;
        for (int i = 0; i < 100; ++i) {
            if (ch_.result(c.seq, r)) {
                canary_.note(canary, r.code == 0, 0);
                if (seat >= 0) seats_.note_order(seat, r.code == 0, now_us());
                log_command(c, r);
                pending = false;
                return true;
            }
            timespec ts{0, 2000000};
            nanosleep(&ts, nullptr);
        }
        r.seq = c.seq;
        r.id = c.id;
        canary_.note(canary, false, 0);
        if (seat >= 0) seats_.note_order(seat, false, now_us());
        log_command(c, r);
        pending = true;
        return true;
    }

    bool post(const pitlane::OpsCommand& cmd, pitlane::HttpResp& resp) noexcept {
        pitlane::OpsResult r{};
        bool pending = false;
        if (!send(cmd, r, pending)) {
            static const char kBusy[] = "{\"code\":4,\"msg\":\"busy\"}";
            resp.json(503, kBusy, sizeof kBusy - 1);
            return true;
        }
        render_result(r, pending);
        resp.json(pending ? 202 : 200, json_, static_cast<uint32_t>(json_len_));
        return true;
    }

    void log_command(const pitlane::OpsCommand& c, const pitlane::OpsResult& r) noexcept {
        char line[pitlane::LogIndex::kText];
        std::snprintf(line, sizeof line, "cmd kind=%u name=%u qty=%lld rc=%d%s%s", static_cast<unsigned>(c.kind),
                      static_cast<unsigned>(c.name), static_cast<long long>(c.qty), static_cast<int>(r.code),
                      r.code ? " " : "", r.code ? r.msg : "accepted");
        const uint8_t kind = c.kind == static_cast<uint8_t>(pitlane::OpsKind::Direct) ||
                                     c.kind == static_cast<uint8_t>(pitlane::OpsKind::Basket) ||
                                     c.kind == static_cast<uint8_t>(pitlane::OpsKind::Algo)
                                 ? pitlane::kLogOrder
                                 : pitlane::kLogOps;
        logs_.add(now_us(), r.code == 0 ? pitlane::kLogInfo : pitlane::kLogWarn, kind, line);
    }

    bool handle(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        const int64_t now = now_us();
        sessions_.gc(now);

        if (std::strcmp(req.path, "/api/health") == 0) {
            static const char kOk[] = "{\"ok\":1}";
            resp.json(200, kOk, sizeof kOk - 1);
            return true;
        }
        if (std::strcmp(req.path, "/api/login") == 0) {
            if (std::strcmp(req.method, "POST") != 0) return method_denied(resp);
            return do_login(req, resp, now);
        }
        if (std::strncmp(req.path, "/api/", 5) != 0) return false;

        char token[65];
        const int sidx = pitlane::ops_token_from(req.auth, req.cookie, token, sizeof token) ? sessions_.find(token, now)
                                                                                            : -1;
        if (sidx < 0) return unauthorized(resp);
        const pitlane::OpsSession& sess = sessions_.at(sidx);

        if (std::strcmp(req.path, "/api/me") == 0) {
            render_me(sess);
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/logout") == 0) {
            sessions_.drop(token);
            static const char kOk[] = "{\"code\":0,\"msg\":\"ok\"}";
            resp.json(200, kOk, sizeof kOk - 1);
            return true;
        }

        if (std::strcmp(req.path, "/api/system") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_system(sess_env(sess));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/tape/meta") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            return tape_meta(req, resp);
        }
        if (std::strcmp(req.path, "/api/tape/rows") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            return tape_rows(req, resp);
        }
        if (std::strcmp(req.path, "/api/tape/describe") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            return tape_describe(req, resp);
        }
        if (std::strcmp(req.path, "/api/research") == 0) {
            if (std::strcmp(req.method, "POST") == 0) {
                if (!allow(sess, pitlane::kPermOpsView, resp)) return true;
                return post_research(req, resp);
            }
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_research();
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/research/apply") == 0) {
            if (std::strcmp(req.method, "POST") != 0) return method_denied(resp);
            if (!allow(sess, pitlane::kPermGateway, resp)) return true;
            return apply_research(resp);
        }
        if (std::strcmp(req.path, "/api/backtest") == 0) {
            if (std::strcmp(req.method, "POST") == 0) {
                if (!allow(sess, pitlane::kPermOpsView, resp)) return true;
                return post_backtest(req, resp);
            }
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_backtest();
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/strategies") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_strategies(sess_env(sess));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/pair") == 0) {
            if (std::strcmp(req.method, "POST") == 0) {
                if (!allow(sess, pitlane::kPermTrade, resp)) return true;
                return post_pair(req, resp);
            }
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_pair(sess_env(sess));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/credit") == 0) {
            if (std::strcmp(req.method, "POST") == 0) {
                if (!allow(sess, pitlane::kPermRiskControl, resp)) return true;
                return post_credit(req, resp);
            }
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_credit(sess_env(sess));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/strategy") == 0) {
            if (std::strcmp(req.method, "POST") != 0) return method_denied(resp);
            if (!allow(sess, pitlane::kPermTrade, resp)) return true;
            return post_strategy(req, resp, sess_env(sess));
        }
        if (std::strcmp(req.path, "/api/status") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            pitlane::StatusSnapshot s{};
            if (!read_snapshot(s)) {
                static const char kBusy[] = "{\"code\":5,\"msg\":\"snapshot busy\"}";
                resp.json(503, kBusy, sizeof kBusy - 1);
                return true;
            }
            render_status(s, sess_env(sess));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/events") == 0) {
            if (!allow(sess, pitlane::kPermOpsView, resp)) return true;
            int64_t since = 0;
            (void)trading_ops::query_i64(req.query, "since", since);
            render_events(since);
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/market/book") == 0 || std::strcmp(req.path, "/api/market/prints") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_market();
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/perf/hops") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_hops();
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/perf/baseline") == 0) {
            if (std::strcmp(req.method, "POST") == 0) {
                if (!allow(sess, pitlane::kPermGateway, resp)) return true;
                collect_perf(base_);
                const bool ok = save_baseline();
                logs_.add(now_us(), ok ? pitlane::kLogInfo : pitlane::kLogError, pitlane::kLogPerf,
                          ok ? "perf baseline captured" : "perf baseline write failed");
                Out o{json_, kJsonCap, 0};
                o.add("{\"code\":%d,\"rows\":%d,\"path\":\"%s\"}", ok ? 0 : 1, base_.count(), base_path_);
                resp.json(ok ? 200 : 500, json_, static_cast<uint32_t>(o.n));
                return true;
            }
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            int64_t tol = 200000;
            (void)trading_ops::query_i64(req.query, "tolerance_ppm", tol);
            render_baseline(tol);
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/perf/flame") == 0) {
            if (std::strcmp(req.method, "POST") == 0) {
                if (!allow(sess, pitlane::kPermOpsView, resp)) return true;
                flame_.reset();
                static const char kOk[] = "{\"code\":0}";
                resp.json(200, kOk, sizeof kOk - 1);
                return true;
            }
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_flame();
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/ops/seats") == 0) {
            if (std::strcmp(req.method, "POST") == 0) {
                if (!allow(sess, pitlane::kPermGateway, resp)) return true;
                return post_seat(req, resp);
            }
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_seats();
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/logs/search") == 0) {
            if (!allow(sess, pitlane::kPermOpsView, resp)) return true;
            char q[64]{};
            char level[12]{};
            char kind[12]{};
            int64_t since = 0;
            int64_t limit = 50;
            (void)trading_ops::query_str(req.query, "q", q, sizeof q);
            (void)trading_ops::query_str(req.query, "level", level, sizeof level);
            (void)trading_ops::query_str(req.query, "kind", kind, sizeof kind);
            (void)trading_ops::query_i64(req.query, "since", since);
            (void)trading_ops::query_i64(req.query, "limit", limit);
            const int8_t lv = pitlane::log_level_id(level);
            const int8_t kd = pitlane::log_kind_id(kind);
            render_logs(q[0] ? q : nullptr, since,
                        lv < 0 ? static_cast<uint8_t>(pitlane::kLogDebug) : static_cast<uint8_t>(lv),
                        kd < 0 ? static_cast<uint8_t>(0xff) : static_cast<uint8_t>(kd), static_cast<int>(limit));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/canary") == 0) {
            if (std::strcmp(req.method, "POST") == 0) {
                if (!allow(sess, pitlane::kPermGateway, resp)) return true;
                return post_canary(req, resp);
            }
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_canary();
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/replay") == 0) {
            if (std::strcmp(req.method, "POST") == 0) {
                if (!allow(sess, pitlane::kPermOpsView, resp)) return true;
                return post_replay(req, resp);
            }
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            int64_t at = 0;
            int64_t limit = 20;
            (void)trading_ops::query_i64(req.query, "at", at);
            (void)trading_ops::query_i64(req.query, "limit", limit);
            render_replay(at, static_cast<int>(limit));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/symbols") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_symbols();
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/series") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            int64_t name = 1, tf_ms = 1000, limit = 240;
            (void)trading_ops::query_i64(req.query, "name", name);
            (void)trading_ops::query_i64(req.query, "tf_ms", tf_ms);
            (void)trading_ops::query_i64(req.query, "limit", limit);
            render_series(static_cast<uint32_t>(name), tf_ms * 1000, static_cast<int>(limit));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/ticks") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            int64_t name = 1, limit = 120;
            (void)trading_ops::query_i64(req.query, "name", name);
            (void)trading_ops::query_i64(req.query, "limit", limit);
            render_ticks(static_cast<uint32_t>(name), static_cast<int>(limit));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/history") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            int64_t name = 1, since = 0, until = INT64_MAX, limit = 300;
            (void)trading_ops::query_i64(req.query, "name", name);
            (void)trading_ops::query_i64(req.query, "since_us", since);
            (void)trading_ops::query_i64(req.query, "until_us", until);
            (void)trading_ops::query_i64(req.query, "limit", limit);
            render_history(static_cast<uint32_t>(name), since, until, static_cast<int>(limit));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/gateways") == 0) {
            if (!allow(sess, pitlane::kPermOpsView, resp)) return true;
            render_gateways(sess_env(sess));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/instruments") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_instruments();
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/accounts") == 0) {
            if (!allow(sess, pitlane::kPermMarket, resp)) return true;
            render_accounts(sess_env(sess));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/risk/precheck") == 0) {
            if (!allow(sess, pitlane::kPermRiskView, resp)) return true;
            Out o{json_, kJsonCap, 0};
            o.add("{\"code\":0");
            add_pre(o);
            o.add("}");
            resp.json(200, json_, static_cast<uint32_t>(o.n));
            return true;
        }
        if (std::strcmp(req.path, "/api/risk/limits") == 0 &&
            std::strcmp(req.method, "POST") != 0) {
            if (!allow(sess, pitlane::kPermRiskView, resp)) return true;
            Out o{json_, kJsonCap, 0};
            o.add("{\"code\":0");
            add_rms(o);
            o.add("}");
            resp.json(200, json_, static_cast<uint32_t>(o.n));
            return true;
        }
        if (std::strcmp(req.path, "/api/alarms") == 0) {
            if (!allow(sess, pitlane::kPermRiskView, resp)) return true;
            int64_t since = 0;
            (void)trading_ops::query_i64(req.query, "since", since);
            render_alarms(since, sess_env(sess));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/ledger") == 0) {
            if (!allow(sess, pitlane::kPermRiskView, resp)) return true;
            render_ledger(req.query);
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/clearing") == 0) {
            if (!allow(sess, pitlane::kPermClear, resp)) return true;
            const uint8_t env = sess_env(sess);
            if (req.query && std::strstr(req.query, "run=1")) {
                char path[640]{};
                if (!write_clear_csv(env, path, sizeof path)) return bad(resp, "cannot write snapshot");
                Out o{json_, kJsonCap, 0};
                o.add("{\"code\":0,\"msg\":\"snapshot written\",\"path\":\"%s\"}", path);
                resp.json(200, json_, static_cast<uint32_t>(o.n));
                return true;
            }
            if (req.query && std::strstr(req.query, "settle=1")) {
                // End-of-day: the snapshot carries the day's numbers, then open
                // lots are marked to the settlement price, today's lots become
                // yesterday's, and the daily counters restart.
                char path[640]{};
                if (!write_clear_csv(env, path, sizeof path)) return bad(resp, "cannot write snapshot");
                const int64_t now = now_us();
                settle_clearing(now, env);
                Out o{json_, kJsonCap, 0};
                o.add("{\"code\":0,\"msg\":\"day settled\",\"path\":\"%s\",\"settle_us\":%lld}", path,
                      static_cast<long long>(now));
                resp.json(200, json_, static_cast<uint32_t>(o.n));
                return true;
            }
            render_clearing(env);
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/clearing/trades") == 0) {
            if (!allow(sess, pitlane::kPermClear, resp)) return true;
            const uint8_t env = sess_env(sess);
            int64_t since = 0;
            (void)trading_ops::query_i64(req.query, "since", since);
            uint32_t want = 0;
            int64_t num = 0;
            if (trading_ops::query_i64(req.query, "acct", num)) want = static_cast<uint32_t>(num);
            render_clear_trades(env, want, since);
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/clearing/history") == 0) {
            if (!allow(sess, pitlane::kPermClear, resp)) return true;
            int64_t days = 8;
            (void)trading_ops::query_i64(req.query, "days", days);
            char acct[24]{};
            (void)trading_ops::query_str(req.query, "acct", acct, sizeof acct);
            const uint32_t want = acct[0] ? accs_.resolve(acct) : 0;
            render_clear_history(sess_env(sess), want, static_cast<int>(days));
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/clearing/period") == 0) {
            if (!allow(sess, pitlane::kPermClear, resp)) return true;
            char acct[24]{};
            (void)trading_ops::query_str(req.query, "acct", acct, sizeof acct);
            const uint32_t want = acct[0] ? accs_.resolve(acct) : 0;
            render_clear_period(sess_env(sess), want);
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/host/health") == 0) {
            if (!allow(sess, pitlane::kPermRiskView, resp)) return true;
            return render_host_health(resp);
        }
        if (std::strcmp(req.path, "/api/users") == 0) {
            if (!allow(sess, pitlane::kPermUsers, resp)) return true;
            render_users();
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }

        if (std::strcmp(req.method, "POST") != 0) return method_denied(resp);

        if (std::strcmp(req.path, "/api/clearing/cash") == 0) {
            if (!allow(sess, pitlane::kPermClear, resp)) return true;
            const uint8_t env = sess_env(sess);
            char action[12]{}, acct[24]{}, ccy[8]{};
            int64_t amount = 0;
            if (!trading_ops::key_str(req.body, req.body_len, "action", action, sizeof action) ||
                !trading_ops::key_i64(req.body, req.body_len, "amount", amount) || amount <= 0) {
                return bad(resp, "action/amount");
            }
            const bool is_deposit = std::strcmp(action, "deposit") == 0;
            const bool is_withdraw = std::strcmp(action, "withdraw") == 0;
            if (!is_deposit && !is_withdraw) return bad(resp, "action");
            (void)trading_ops::key_str(req.body, req.body_len, "acct", acct, sizeof acct);
            (void)trading_ops::key_str(req.body, req.body_len, "ccy", ccy, sizeof ccy);
            uint32_t num = acct[0] ? accs_.resolve(acct) : 0;
            if (num == 0) {
                for (int i = 0; i < accs_.count(); ++i) {
                    if (accs_.spec(i).env == env) {
                        num = accs_.spec(i).num;
                        break;
                    }
                }
            }
            if (num == 0 || !acct_env_ok(num, env)) return mode_denied(resp, "account not in this mode");
            const int ai = accs_.index_of_num(num);
            const char* code = ccy[0] ? ccy : (ai >= 0 ? accs_.spec(ai).ccy : "CNY");
            int64_t seq = 0;
            if (!submit_cash(num, is_withdraw ? pitlane::kClrWithdraw : pitlane::kClrDeposit, code, amount, seq)) {
                static const char kBusy[] = "{\"code\":4,\"msg\":\"cash queue full\"}";
                resp.json(503, kBusy, sizeof kBusy - 1);
                return true;
            }
            Out o{json_, kJsonCap, 0};
            o.add("{\"code\":0,\"msg\":\"queued\",\"seq\":%lld,\"acct\":%u}", static_cast<long long>(seq),
                  static_cast<unsigned>(num));
            resp.json(200, json_, static_cast<uint32_t>(o.n));
            return true;
        }

        if (std::strcmp(req.path, "/api/clearing/corp") == 0) {
            if (!allow(sess, pitlane::kPermClear, resp)) return true;
            const uint8_t env = sess_env(sess);
            int64_t name = 0;
            int64_t event_id = 0;
            int64_t kind = 0;
            if (!trading_ops::key_i64(req.body, req.body_len, "name", name) || name <= 0 ||
                !trading_ops::key_i64(req.body, req.body_len, "event_id", event_id) || event_id <= 0 ||
                !trading_ops::key_i64(req.body, req.body_len, "kind", kind) || kind < 1 || kind > 3) {
                return bad(resp, "name/event_id/kind");
            }
            CashJob j{};
            j.name = static_cast<uint32_t>(name);
            j.event_id = event_id;
            j.corp_kind = static_cast<uint8_t>(kind);
            j.per10_cash = j.per10_stock = 0;
            j.num = 1;
            j.den = 1;
            j.tax_ppm = 100000;
            (void)trading_ops::key_i64(req.body, req.body_len, "per10_cash", j.per10_cash);
            (void)trading_ops::key_i64(req.body, req.body_len, "per10_stock", j.per10_stock);
            (void)trading_ops::key_i64(req.body, req.body_len, "num", j.num);
            (void)trading_ops::key_i64(req.body, req.body_len, "den", j.den);
            (void)trading_ops::key_i64(req.body, req.body_len, "tax_ppm", j.tax_ppm);
            if (j.num <= 0 || j.den <= 0 || j.tax_ppm < 0) return bad(resp, "ratio/tax");
            j.acct = body_acct(req, env);
            if (j.acct == 0) return mode_denied(resp, "account not in this mode");
            j.kind = kJobCorp;
            if (!enqueue_book(j)) {
                static const char kBusy[] = "{\"code\":4,\"msg\":\"book queue full\"}";
                resp.json(503, kBusy, sizeof kBusy - 1);
                return true;
            }
            Out o{json_, kJsonCap, 0};
            o.add("{\"code\":0,\"msg\":\"queued\",\"seq\":%lld,\"acct\":%u}", static_cast<long long>(j.seq),
                  static_cast<unsigned>(j.acct));
            resp.json(200, json_, static_cast<uint32_t>(o.n));
            return true;
        }

        if (std::strcmp(req.path, "/api/clearing/expense") == 0) {
            if (!allow(sess, pitlane::kPermClear, resp)) return true;
            const uint8_t env = sess_env(sess);
            char action[16]{};
            int64_t amount = 0;
            if (!trading_ops::key_str(req.body, req.body_len, "action", action, sizeof action) ||
                !trading_ops::key_i64(req.body, req.body_len, "amount", amount) || amount == 0) {
                return bad(resp, "action/amount");
            }
            CashJob j{};
            if (std::strcmp(action, "interest") == 0) {
                j.kind = kJobInterest;
            } else if (std::strcmp(action, "mgmt") == 0) {
                j.kind = kJobExpense;
                j.expense_kind = pitlane::kClrMgmtFee;
            } else if (std::strcmp(action, "custody") == 0) {
                j.kind = kJobExpense;
                j.expense_kind = pitlane::kClrCustodyFee;
            } else if (std::strcmp(action, "sales") == 0) {
                j.kind = kJobExpense;
                j.expense_kind = pitlane::kClrSalesFee;
            } else if (std::strcmp(action, "other") == 0) {
                j.kind = kJobExpense;
                j.expense_kind = pitlane::kClrOtherFee;
            } else {
                return bad(resp, "action");
            }
            j.amount = amount;
            j.acct = body_acct(req, env);
            if (j.acct == 0) return mode_denied(resp, "account not in this mode");
            if (!enqueue_book(j)) {
                static const char kBusy[] = "{\"code\":4,\"msg\":\"book queue full\"}";
                resp.json(503, kBusy, sizeof kBusy - 1);
                return true;
            }
            Out o{json_, kJsonCap, 0};
            o.add("{\"code\":0,\"msg\":\"queued\",\"seq\":%lld,\"acct\":%u}", static_cast<long long>(j.seq),
                  static_cast<unsigned>(j.acct));
            resp.json(200, json_, static_cast<uint32_t>(o.n));
            return true;
        }

        if (std::strcmp(req.path, "/api/clearing/shares") == 0) {
            if (!allow(sess, pitlane::kPermClear, resp)) return true;
            const uint8_t env = sess_env(sess);
            char action[16]{};
            int64_t shares = 0;
            if (!trading_ops::key_str(req.body, req.body_len, "action", action, sizeof action) ||
                !trading_ops::key_i64(req.body, req.body_len, "shares", shares) || shares <= 0) {
                return bad(resp, "action/shares");
            }
            CashJob j{};
            if (std::strcmp(action, "subscribe") == 0) {
                j.kind = kJobSubscribe;
            } else if (std::strcmp(action, "redeem") == 0) {
                j.kind = kJobRedeem;
            } else {
                return bad(resp, "action");
            }
            j.amount = shares;
            j.acct = body_acct(req, env);
            if (j.acct == 0) return mode_denied(resp, "account not in this mode");
            if (!enqueue_book(j)) {
                static const char kBusy[] = "{\"code\":4,\"msg\":\"book queue full\"}";
                resp.json(503, kBusy, sizeof kBusy - 1);
                return true;
            }
            Out o{json_, kJsonCap, 0};
            o.add("{\"code\":0,\"msg\":\"queued\",\"seq\":%lld,\"acct\":%u}", static_cast<long long>(j.seq),
                  static_cast<unsigned>(j.acct));
            resp.json(200, json_, static_cast<uint32_t>(o.n));
            return true;
        }

        if (std::strcmp(req.path, "/api/clearing/reconcile") == 0) {
            if (!allow(sess, pitlane::kPermClear, resp)) return true;
            const uint8_t env = sess_env(sess);
            char pa[512]{}, pb[512]{};
            if (!trading_ops::key_str(req.body, req.body_len, "statement_a", pa, sizeof pa) || !tape_path_ok(pa)) {
                return bad(resp, "statement_a");
            }
            (void)trading_ops::key_str(req.body, req.body_len, "statement_b", pb, sizeof pb);
            if (pb[0] && !tape_path_ok(pb)) return bad(resp, "statement_b");
            const uint32_t num = body_acct(req, env);
            if (num == 0) return mode_denied(resp, "account not in this mode");
            const int bi = accs_.index_of_num(num);
            pitlane::ClearingReport rep{};
            if (bi < 0 || bi >= 8 || !clear_board_[bi].read(rep)) return bad(resp, "no clearing snapshot");
            pitlane::StmtBook a{};
            pitlane::StmtBook b{};
            if (!a.load_csv(pa)) return bad(resp, "cannot read statement_a");
            if (pb[0] && !b.load_csv(pb)) return bad(resp, "cannot read statement_b");
            int64_t qty_tol = 0;
            int64_t money_tol = 0;
            (void)trading_ops::key_i64(req.body, req.body_len, "qty_tol", qty_tol);
            (void)trading_ops::key_i64(req.body, req.body_len, "money_tol", money_tol);
            pitlane::ReconOut out{};
            (void)pitlane::recon_three_way(rep, &a, pb[0] ? &b : nullptr, qty_tol, money_tol, out);
            char dir[512]{};
            trading_ops::conf_sibling(conf_path_, "", dir, sizeof dir);
            std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_r(&t, &tm);
            char path[640];
            std::snprintf(path, sizeof path, "%srecon-%04d%02d%02d-%02d%02d%02d-%s.csv", dir, tm.tm_year + 1900,
                          tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                          pitlane::acc_env_name(env));
            if (!pitlane::recon_csv(out, path)) path[0] = '\0';
            render_recon(out, path);
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }

        if (std::strcmp(req.path, "/api/user") == 0) {
            if (!allow(sess, pitlane::kPermUsers, resp)) return true;
            return do_user(req, resp);
        }
        if (std::strcmp(req.path, "/api/gateway") == 0) {
            if (!allow(sess, pitlane::kPermGateway, resp)) return true;
            char action[16]{}, id[24]{};
            if (!trading_ops::key_str(req.body, req.body_len, "action", action, sizeof action) ||
                !trading_ops::key_str(req.body, req.body_len, "id", id, sizeof id)) {
                return bad(resp, "action/id");
            }
            pitlane::OpsCommand c{};
            if (std::strcmp(action, "start") == 0) c.kind = static_cast<uint8_t>(pitlane::OpsKind::GatewayStart);
            else if (std::strcmp(action, "stop") == 0) c.kind = static_cast<uint8_t>(pitlane::OpsKind::GatewayStop);
            else return bad(resp, "action");
            const int gi = gw_.index_of(id);
            if (gi < 0) return bad(resp, "id");
            if (gw_.spec(gi).env_bit != sess_env(sess)) return mode_denied(resp, "gateway in another mode");
            std::snprintf(c.target, sizeof c.target, "%s", id);
            c.env = sess_env(sess);
            return post(c, resp);
        }

        pitlane::OpsCommand c{};
        if (std::strcmp(req.path, "/api/order") == 0) {
            if (!allow(sess, pitlane::kPermTrade, resp)) return true;
            int64_t name = 0;
            int64_t side = 0;
            int64_t qty = 0;
            if (!trading_ops::key_i64(req.body, req.body_len, "name", name) ||
                !trading_ops::key_i64(req.body, req.body_len, "side", side) ||
                !trading_ops::key_i64(req.body, req.body_len, "qty", qty)) {
                return bad(resp, "name/side/qty");
            }
            c.kind = static_cast<uint8_t>(pitlane::OpsKind::Direct);
            c.acct = order_acct(sess, req, resp);
            if (c.acct == 0) return true;
            c.env = sess_env(sess);
            c.name = static_cast<uint32_t>(name);
            c.side = static_cast<uint8_t>(side);
            c.qty = qty;
            (void)trading_ops::key_i64(req.body, req.body_len, "px", c.px);
            int64_t off = 0;
            (void)trading_ops::key_i64(req.body, req.body_len, "off", off);
            c.off = static_cast<uint8_t>(off < 0 || off > 6 ? 0 : off);
            int64_t biz = 0;
            (void)trading_ops::key_i64(req.body, req.body_len, "biz", biz);
            if (biz < 0 || biz > static_cast<int64_t>(pitlane::Biz::MergeOrder)) return bad(resp, "biz");
            c.biz = static_cast<uint8_t>(biz);
            return post(c, resp);
        }
        if (std::strcmp(req.path, "/api/risk/limits") == 0) {
            if (!allow(sess, pitlane::kPermRiskControl, resp)) return true;
            c = pitlane::OpsCommand{};
            c.kind = static_cast<uint8_t>(pitlane::OpsKind::RiskLimits);
            c.env = sess_env(sess);
            pitlane::StatusSnapshot s{};
            if (!read_snapshot(s)) return bad(resp, "snapshot");
            c.limits.max_order_qty = s.risk.max_order_qty;
            c.limits.max_order_notional = s.risk.max_order_notional;
            c.limits.max_pos_long = s.risk.max_pos_long;
            c.limits.max_pos_short = s.risk.max_pos_short;
            c.limits.max_daily_notional = s.risk.max_daily_notional;
            c.limits.price_band_ppm = s.risk.price_band_ppm;
            c.limits.tick_size = s.risk.tick_size;
            c.limits.lot_size = s.risk.lot_size;
            c.limits.max_orders_per_sec = s.risk.max_orders_per_sec;
            c.limits.max_live_orders = s.risk.max_live_orders;
            int64_t per_sec = c.limits.max_orders_per_sec;
            int64_t live = c.limits.max_live_orders;
            const auto take = [&](const char* key, int64_t& dst) -> bool {
                int64_t v = 0;
                if (!trading_ops::key_i64(req.body, req.body_len, key, v)) return true;
                if (v < 0) return false;
                dst = v;
                return true;
            };
            if (!take("max_order_qty", c.limits.max_order_qty) ||
                !take("max_order_notional", c.limits.max_order_notional) ||
                !take("max_pos_long", c.limits.max_pos_long) ||
                !take("max_pos_short", c.limits.max_pos_short) ||
                !take("max_daily_notional", c.limits.max_daily_notional) ||
                !take("price_band_ppm", c.limits.price_band_ppm) ||
                !take("tick_size", c.limits.tick_size) || !take("lot_size", c.limits.lot_size) ||
                !take("max_orders_per_sec", per_sec) || !take("max_live_orders", live)) {
                return bad(resp, "risk limits");
            }
            c.limits.max_orders_per_sec = static_cast<int32_t>(per_sec > 1000000 ? 1000000 : per_sec);
            c.limits.max_live_orders = static_cast<int32_t>(live > 1000000 ? 1000000 : live);
            return post(c, resp);
        }
        if (std::strcmp(req.path, "/api/netting") == 0) {
            if (!allow(sess, pitlane::kPermTrade, resp)) return true;
            pitlane::NetOrder rows[trading_ops::kNetRows]{};
            const int rn = trading_ops::key_net_rows(req.body, req.body_len, rows, trading_ops::kNetRows);
            if (rn <= 0) return bad(resp, "rows");
            int64_t scope = 1;
            int64_t lot = 1;
            int64_t apply = 0;
            (void)trading_ops::key_i64(req.body, req.body_len, "scope", scope);
            (void)trading_ops::key_i64(req.body, req.body_len, "lot", lot);
            (void)trading_ops::key_i64(req.body, req.body_len, "apply", apply);
            const uint32_t def = order_acct(sess, req, resp);
            if (def == 0) return true;
            pitlane::NetCfg cfg{};
            cfg.scope = static_cast<uint8_t>(scope);
            cfg.lot = lot;
            pitlane::NetBook<trading_ops::kNetRows> book;
            if (!book.open(cfg)) return bad(resp, "scope/lot");
            for (int i = 0; i < rn; ++i) {
                if (rows[i].acct == 0) rows[i].acct = def;
                if (!acct_env_ok(rows[i].acct, sess_env(sess))) return mode_denied(resp, "account not in this mode");
                (void)book.add(rows[i]);
            }
            pitlane::NetLeg legs[trading_ops::kNetRows]{};
            pitlane::NetReport rep{};
            const int nl = book.legs(legs, trading_ops::kNetRows, rep);
            if (nl < 0) return bad(resp, "legs");
            int sent = 0;
            if (apply) {
                for (int i = 0; i < nl; ++i) {
                    pitlane::OpsCommand c{};
                    c.kind = static_cast<uint8_t>(pitlane::OpsKind::Direct);
                    c.acct = legs[i].acct;
                    c.env = sess_env(sess);
                    c.name = legs[i].name;
                    c.side = legs[i].side;
                    c.qty = legs[i].qty;
                    pitlane::OpsResult r{};
                    bool pending = false;
                    if (!send(c, r, pending) || r.code != 0) break;
                    ++sent;
                }
            }
            Out o{json_, kJsonCap, 0};
            o.add("{\"code\":0,\"msg\":\"ok\",\"gross\":%lld,\"net\":%lld,\"residual\":%lld,\"saved\":%lld,"
                  "\"rows_in\":%d,\"rows_out\":%d,\"dropped\":%d,\"invalid\":%d,\"overflow\":%d,"
                  "\"sent\":%d,\"legs\":[",
                  static_cast<long long>(rep.gross), static_cast<long long>(rep.net),
                  static_cast<long long>(rep.residual), static_cast<long long>(rep.saved), rep.rows_in,
                  rep.rows_out, rep.dropped, rep.invalid, rep.overflow, sent);
            for (int i = 0; i < nl; ++i) {
                o.add("%s{\"acct\":%u,\"counter\":%u,\"name\":%u,\"side\":%u,\"qty\":%lld}", i ? "," : "",
                      static_cast<unsigned>(legs[i].acct), static_cast<unsigned>(legs[i].counter),
                      static_cast<unsigned>(legs[i].name), static_cast<unsigned>(legs[i].side),
                      static_cast<long long>(legs[i].qty));
            }
            o.add("]}");
            resp.json(200, json_, static_cast<uint32_t>(o.n));
            return true;
        }
        if (std::strcmp(req.path, "/api/unlock") == 0) {
            if (!allow(sess, pitlane::kPermTrade, resp)) return true;
            int64_t name = 0;
            int64_t long_qty = 0;
            int64_t short_qty = 0;
            if (!trading_ops::key_i64(req.body, req.body_len, "name", name) ||
                !trading_ops::key_i64(req.body, req.body_len, "long_qty", long_qty) ||
                !trading_ops::key_i64(req.body, req.body_len, "short_qty", short_qty)) {
                return bad(resp, "name/long_qty/short_qty");
            }
            int64_t lot = 1;
            int64_t clip = 0;
            int64_t px = 0;
            int64_t apply = 0;
            (void)trading_ops::key_i64(req.body, req.body_len, "lot", lot);
            (void)trading_ops::key_i64(req.body, req.body_len, "clip", clip);
            (void)trading_ops::key_i64(req.body, req.body_len, "px", px);
            (void)trading_ops::key_i64(req.body, req.body_len, "apply", apply);
            const uint32_t acct = order_acct(sess, req, resp);
            if (acct == 0) return true;
            pitlane::UnlockCfg cfg{};
            cfg.lot = lot;
            cfg.clip = clip;
            pitlane::UnlockPlan plan;
            if (!plan.open(cfg, long_qty, short_qty)) return bad(resp, "lot/long_qty/short_qty");
            pitlane::UnlockReq want[2]{};
            const int wn = plan.step(want, 2);
            int sent = 0;
            if (apply) {
                for (int i = 0; i < wn; ++i) {
                    pitlane::OpsCommand c{};
                    c.kind = static_cast<uint8_t>(pitlane::OpsKind::Direct);
                    c.acct = acct;
                    c.env = sess_env(sess);
                    c.name = static_cast<uint32_t>(name);
                    c.side = want[i].side;
                    c.qty = want[i].qty;
                    c.px = px;
                    pitlane::OpsResult r{};
                    bool pending = false;
                    if (!send(c, r, pending) || r.code != 0) break;
                    ++sent;
                }
            }
            Out o{json_, kJsonCap, 0};
            o.add("{\"code\":0,\"msg\":\"ok\",\"goal\":%lld,\"residual_long\":%lld,\"residual_short\":%lld,"
                  "\"sent\":%d,\"legs\":[",
                  static_cast<long long>(plan.goal()), static_cast<long long>(long_qty - plan.goal()),
                  static_cast<long long>(short_qty - plan.goal()), sent);
            for (int i = 0; i < wn; ++i) {
                o.add("%s{\"leg\":%u,\"side\":%u,\"qty\":%lld}", i ? "," : "",
                      static_cast<unsigned>(want[i].leg), static_cast<unsigned>(want[i].side),
                      static_cast<long long>(want[i].qty));
            }
            o.add("]}");
            resp.json(200, json_, static_cast<uint32_t>(o.n));
            return true;
        }
        if (std::strcmp(req.path, "/api/pair/order") == 0) {
            if (!allow(sess, pitlane::kPermTrade, resp)) return true;
            int64_t an = 0, as = 0, aq = 0, ap = 0;
            int64_t bn = 0, bs = 0, bq = 0, bp = 0;
            if (!trading_ops::key_i64(req.body, req.body_len, "a_name", an) ||
                !trading_ops::key_i64(req.body, req.body_len, "a_side", as) ||
                !trading_ops::key_i64(req.body, req.body_len, "a_qty", aq) ||
                !trading_ops::key_i64(req.body, req.body_len, "b_name", bn) ||
                !trading_ops::key_i64(req.body, req.body_len, "b_side", bs) ||
                !trading_ops::key_i64(req.body, req.body_len, "b_qty", bq)) {
                return bad(resp, "a_name/a_side/a_qty/b_name/b_side/b_qty");
            }
            if (an <= 0 || bn <= 0 || aq <= 0 || bq <= 0 || (as != 1 && as != 2) || (bs != 1 && bs != 2)) {
                return bad(resp, "pair legs");
            }
            int64_t fx = 0;
            int64_t apply = 0;
            (void)trading_ops::key_i64(req.body, req.body_len, "a_px", ap);
            (void)trading_ops::key_i64(req.body, req.body_len, "b_px", bp);
            (void)trading_ops::key_i64(req.body, req.body_len, "fx_ppm", fx);
            (void)trading_ops::key_i64(req.body, req.body_len, "apply", apply);
            const uint32_t acct = order_acct(sess, req, resp);
            if (acct == 0) return true;
            int64_t spread = 0;
            if (ap > 0 && bp > 0 && fx > 0) {
                const int64_t b_in_a = (bp * fx + 500000) / 1000000;
                spread = (ap - b_in_a) * 1000000 / ap;
            }
            int sent = 0;
            if (apply) {
                const int64_t px[2] = {ap, bp};
                const int64_t name[2] = {an, bn};
                const int64_t side[2] = {as, bs};
                const int64_t qty[2] = {aq, bq};
                for (int i = 0; i < 2; ++i) {
                    pitlane::OpsCommand c{};
                    c.kind = static_cast<uint8_t>(pitlane::OpsKind::Direct);
                    c.acct = acct;
                    c.env = sess_env(sess);
                    c.name = static_cast<uint32_t>(name[i]);
                    c.side = static_cast<uint8_t>(side[i]);
                    c.qty = qty[i];
                    c.px = px[i];
                    pitlane::OpsResult r{};
                    bool pending = false;
                    if (!send(c, r, pending) || r.code != 0) break;
                    ++sent;
                }
            }
            Out o{json_, kJsonCap, 0};
            o.add("{\"code\":0,\"msg\":\"ok\",\"sent\":%d,\"spread_ppm\":%lld}", sent, static_cast<long long>(spread));
            resp.json(200, json_, static_cast<uint32_t>(o.n));
            return true;
        }
        if (std::strcmp(req.path, "/api/basket") == 0) {
            if (!allow(sess, pitlane::kPermTrade, resp)) return true;
            const int n = trading_ops::key_legs(req.body, req.body_len, c.basket, pitlane::kOpsBasketLegs);
            if (n <= 0) return bad(resp, "legs");
            c.kind = static_cast<uint8_t>(pitlane::OpsKind::Basket);
            c.acct = order_acct(sess, req, resp);
            if (c.acct == 0) return true;
            c.env = sess_env(sess);
            c.legs = static_cast<uint16_t>(n);
            int64_t id = 0;
            if (trading_ops::key_i64(req.body, req.body_len, "id", id) && id > 0) c.id = static_cast<uint64_t>(id);
            return post(c, resp);
        }
        if (std::strcmp(req.path, "/api/algo") == 0) {
            if (!allow(sess, pitlane::kPermTrade, resp)) return true;
            char action[16];
            const bool has_action = trading_ops::key_str(req.body, req.body_len, "action", action, sizeof action);
            c.env = sess_env(sess);
            if (has_action && std::strcmp(action, "create") != 0) {
                int64_t id = 0;
                if (!trading_ops::key_i64(req.body, req.body_len, "id", id) || id <= 0) {
                    return bad(resp, "id");
                }
                c.id = static_cast<uint64_t>(id);
                if (std::strcmp(action, "pause") == 0) c.kind = static_cast<uint8_t>(pitlane::OpsKind::AlgoPause);
                else if (std::strcmp(action, "resume") == 0) c.kind = static_cast<uint8_t>(pitlane::OpsKind::AlgoResume);
                else if (std::strcmp(action, "cancel") == 0) c.kind = static_cast<uint8_t>(pitlane::OpsKind::AlgoCancel);
                else if (std::strcmp(action, "update") == 0) {
                    c.kind = static_cast<uint8_t>(pitlane::OpsKind::AlgoUpdate);
                    int64_t qty = 0;
                    int64_t horizon_ms = 0;
                    int64_t slice_ms = 0;
                    (void)trading_ops::key_i64(req.body, req.body_len, "qty", qty);
                    (void)trading_ops::key_i64(req.body, req.body_len, "horizon_ms", horizon_ms);
                    (void)trading_ops::key_i64(req.body, req.body_len, "slice_ms", slice_ms);
                    if (qty < 0 || horizon_ms < 0 || slice_ms < 0) return bad(resp, "update values");
                    c.qty = qty;
                    c.horizon_us = horizon_ms * 1000;
                    c.slice_us = slice_ms * 1000;
                } else {
                    return bad(resp, "action");
                }
                return post(c, resp);
            }
            int64_t name = 0;
            int64_t side = 0;
            int64_t qty = 0;
            int64_t horizon_ms = 0;
            int64_t slice_ms = 0;
            if (!trading_ops::key_i64(req.body, req.body_len, "name", name) ||
                !trading_ops::key_i64(req.body, req.body_len, "side", side) ||
                !trading_ops::key_i64(req.body, req.body_len, "qty", qty) ||
                !trading_ops::key_i64(req.body, req.body_len, "horizon_ms", horizon_ms)) {
                return bad(resp, "name/side/qty/horizon_ms");
            }
            if (!trading_ops::key_i64(req.body, req.body_len, "slice_ms", slice_ms)) slice_ms = horizon_ms / 10;
            if (slice_ms <= 0) slice_ms = 1;
            int64_t t0 = 0;
            char kind[16];
            (void)trading_ops::key_i64(req.body, req.body_len, "t0", t0);
            const bool t0_kind = trading_ops::key_str(req.body, req.body_len, "kind", kind, sizeof kind) &&
                                 std::strcmp(kind, "t0") == 0;
            c.kind = static_cast<uint8_t>(pitlane::OpsKind::Algo);
            c.acct = order_acct(sess, req, resp);
            if (c.acct == 0) return true;
            c.name = static_cast<uint32_t>(name);
            c.side = static_cast<uint8_t>(side);
            c.qty = qty;
            c.horizon_us = horizon_ms * 1000;
            c.slice_us = slice_ms * 1000;
            c.mode = (t0 != 0 || t0_kind) ? 1 : 0;
            return post(c, resp);
        }
        if (std::strcmp(req.path, "/api/control") == 0) {
            char action[32];
            if (!trading_ops::key_str(req.body, req.body_len, "action", action, sizeof action)) {
                return bad(resp, "action");
            }
            pitlane::OpsKind kind;
            uint32_t need = pitlane::kPermRiskControl;
            if (std::strcmp(action, "pause") == 0) kind = pitlane::OpsKind::Pause;
            else if (std::strcmp(action, "resume") == 0) kind = pitlane::OpsKind::Resume;
            else if (std::strcmp(action, "halt") == 0) kind = pitlane::OpsKind::Halt;
            else if (std::strcmp(action, "resume_risk") == 0) kind = pitlane::OpsKind::ResumeRisk;
            else if (std::strcmp(action, "reload") == 0) {
                kind = pitlane::OpsKind::Reload;
                need = pitlane::kPermGateway;
            } else if (std::strcmp(action, "stop") == 0) {
                if (sess_env(sess) != pitlane::kEnvLive) return mode_denied(resp, "stop requires live mode");
                kind = pitlane::OpsKind::Stop;
            } else if (std::strcmp(action, "cancel_all") == 0) kind = pitlane::OpsKind::CancelAll;
            else {
                Out o{json_, kJsonCap, 0};
                o.add("{\"code\":%d,\"msg\":\"unknown action\"}", static_cast<int>(pitlane::kOpsBadReq));
                resp.json(400, json_, static_cast<uint32_t>(o.n));
                return true;
            }
            if (!allow(sess, need, resp)) return true;
            c.kind = static_cast<uint8_t>(kind);
            c.env = sess_env(sess);
            return post(c, resp);
        }
        return false;
    }

    // ---- lifecycle endpoints ----

    // Pre-trade rules and rolling counters, shared by /api/system and
    // /api/risk/precheck.
    void add_pre(Out& o) noexcept {
        const pitlane::PreLimits& l = pre_.limits();
        const int64_t now = now_us();
        o.add(",\"pre\":{\"orders_1s\":%d,\"orders_10s\":%d,\"market_1s\":%d,\"cancel_ratio_ppm\":%d,"
              "\"cancel_amount_ppm\":%d,\"min_ratio_orders\":%d,\"amount_window_s\":%d,\"self_trade\":%u,"
              "\"open_list\":%d,\"close_list\":%d,\"active\":%d,\"halted\":%u,\"notional\":[",
              static_cast<int>(l.max_orders_1s), static_cast<int>(l.max_orders_10s),
              static_cast<int>(l.max_market_orders_1s), static_cast<int>(l.max_cancel_ratio_ppm),
              static_cast<int>(l.max_cancel_amount_ratio_ppm), static_cast<int>(l.min_orders_for_ratio),
              static_cast<int>(l.cancel_amount_window_s), static_cast<unsigned>(l.self_trade_block),
              pre_.open_blacklist().size(), pre_.close_blacklist().size(), pre_.active_count(),
              static_cast<unsigned>(pre_.halted()));
        for (int i = 0; i < 8; ++i) o.add("%s%lld", i ? "," : "", static_cast<long long>(l.max_notional[i]));
        o.add("],\"accts\":[");
        const int na = accs_.count();
        for (int i = 0; i < na; ++i) {
            const uint32_t a = accs_.spec(i).num;
            o.add("%s{\"acct\":%u,\"o1\":%lld,\"o10\":%lld,\"c10\":%lld,\"n10\":%lld,\"ratio\":%lld}", i ? "," : "",
                  a, static_cast<long long>(pre_.flow().orders(a, 1, now)),
                  static_cast<long long>(pre_.flow().orders(a, 10, now)),
                  static_cast<long long>(pre_.flow().cancels(a, 10, now)),
                  static_cast<long long>(pre_.flow().notional(a, 10, now)),
                  static_cast<long long>(pre_.flow().cancel_ratio_ppm(a)));
        }
        o.add("]}");
    }

    // Live Rms limits and the two in-process layer flags, shared by
    // /api/system and /api/risk/limits.
    void add_rms(Out& o) noexcept {
        pitlane::StatusSnapshot s{};
        if (!read_snapshot(s)) {
            o.add(",\"rms\":{}");
            return;
        }
        const pitlane::StatusRisk& r = s.risk;
        o.add(",\"rms\":{\"max_order_qty\":%lld,\"max_order_notional\":%lld,\"max_pos_long\":%lld,"
              "\"max_pos_short\":%lld,\"max_daily_notional\":%lld,\"price_band_ppm\":%lld,\"tick_size\":%lld,"
              "\"lot_size\":%lld,\"max_orders_per_sec\":%d,\"max_live_orders\":%d,\"halted\":%u,\"precheck\":%u}",
              static_cast<long long>(r.max_order_qty), static_cast<long long>(r.max_order_notional),
              static_cast<long long>(r.max_pos_long), static_cast<long long>(r.max_pos_short),
              static_cast<long long>(r.max_daily_notional), static_cast<long long>(r.price_band_ppm),
              static_cast<long long>(r.tick_size), static_cast<long long>(r.lot_size),
              static_cast<int>(r.max_orders_per_sec), static_cast<int>(r.max_live_orders),
              static_cast<unsigned>(r.halted), static_cast<unsigned>(r.precheck));
    }

    void render_system(uint8_t env) noexcept {
        pitlane::StatusSnapshot s{};
        const bool snapped = read_snapshot(s);
        int gw_total = 0;
        int gw_on = 0;
        int gw_bad = 0;
        for (int i = 0; i < gw_.count(); ++i) {
            if (gw_.spec(i).env_bit != env) continue;
            ++gw_total;
            const uint8_t st = gw_.status(i).state;
            if (st == pitlane::kGwRunning) ++gw_on;
            else if (st == pitlane::kGwFailed) ++gw_bad;
        }
        int st_total = 0;
        int st_on = 0;
        for (int i = 0; i < pitlane::StrategyRegistry::kMaxStrategies; ++i) {
            pitlane::StratRow r{};
            if (!strategies_.read(i, r) || !r.id[0] || r.env != env) continue;
            ++st_total;
            if (r.state == pitlane::kStratRunning) ++st_on;
        }
        trading_jobs::ResearchResult rr{};
        trading_jobs::BacktestResult br{};
        const bool has_rr = jobs_.research(rr);
        const bool has_br = jobs_.backtest(br);
        pitlane::Alarm al[pitlane::AlarmBoard::kCap]{};
        const int an = alarms_.recent(al, pitlane::AlarmBoard::kCap, 0, env, true);
        int a_crit = 0;
        int a_warn = 0;
        for (int i = 0; i < an; ++i) {
            if (al[i].sev == pitlane::kAlarmCritical) ++a_crit;
            else if (al[i].sev == pitlane::kAlarmWarn) ++a_warn;
        }
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"state\":%u,\"halted\":%u,\"mode\":\"%s\",\"md_up\":%u,\"td_up\":%u,"
              "\"quotes\":%lld,\"equity\":%lld,\"drawdown\":%lld,\"uptime_us\":%lld,\"snapshot\":%u,"
              "\"gateways\":{\"total\":%d,\"running\":%d,\"failed\":%d},"
              "\"strategies\":{\"total\":%d,\"running\":%d},"
              "\"research\":{\"state\":\"%s\",\"id\":%llu,\"rows\":%d},"
              "\"backtest\":{\"state\":\"%s\",\"id\":%llu},"
              "\"alarms\":{\"critical\":%d,\"warn\":%d},\"web\":{\"port\":%u}",
              static_cast<unsigned>(s.state), static_cast<unsigned>(s.halted), pitlane::acc_env_name(env),
              static_cast<unsigned>(s.md_up), static_cast<unsigned>(s.td_up),
              static_cast<long long>(s.quotes), static_cast<long long>(s.equity),
              static_cast<long long>(s.drawdown), static_cast<long long>(s.uptime_us), snapped ? 1u : 0u,
              gw_total, gw_on, gw_bad, st_total, st_on,
              has_rr ? trading_jobs::job_state_name(rr.state) : "idle",
              static_cast<unsigned long long>(has_rr ? rr.id : 0), has_rr ? rr.rows_n : 0,
              has_br ? trading_jobs::job_state_name(br.state) : "idle",
              static_cast<unsigned long long>(has_br ? br.id : 0), a_crit, a_warn,
              static_cast<unsigned>(http_.port()));
        add_pre(o);
        add_rms(o);
        o.add("}");
        json_len_ = o.n;
    }

    void render_research() noexcept {
        trading_jobs::ResearchResult r{};
        const bool have = jobs_.research(r);
        const uint64_t req = jobs_.research_req_id();
        if (req != 0 && (!have || r.id != req)) {
            std::snprintf(json_, kJsonCap,
                          "{\"code\":0,\"id\":%llu,\"state\":\"queued\",\"started_us\":0,\"done_us\":0,"
                          "\"rows_n\":0,\"note\":\"\",\"best\":{\"kind\":0,\"window\":0,\"horizon\":0,"
                          "\"sign\":1,\"threshold_ppm\":0,\"n\":0,\"ic\":0,\"rank_ic\":0,\"hit\":0,"
                          "\"turnover\":0,\"score\":0},\"rows\":[]}",
                          static_cast<unsigned long long>(req));
            json_len_ = static_cast<int>(std::strlen(json_));
            return;
        }
        if (!have) {
            static const char kIdle[] = "{\"code\":0,\"state\":\"idle\",\"id\":0,\"rows_n\":0,\"rows\":[]}";
            std::snprintf(json_, kJsonCap, "%s", kIdle);
            json_len_ = static_cast<int>(std::strlen(json_));
            return;
        }
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"id\":%llu,\"state\":\"%s\",\"started_us\":%lld,\"done_us\":%lld,"
              "\"rows_n\":%d,\"note\":\"%s\",\"best\":{\"kind\":%u,\"window\":%u,\"horizon\":%d,"
              "\"sign\":%d,\"threshold_ppm\":%lld,\"n\":%lld,\"ic\":%lld,\"rank_ic\":%lld,\"hit\":%lld,"
              "\"turnover\":%lld,\"score\":%lld},\"rows\":[",
              static_cast<unsigned long long>(r.id), trading_jobs::job_state_name(r.state),
              static_cast<long long>(r.started_us), static_cast<long long>(r.done_us), r.rows_n, r.note,
              static_cast<unsigned>(r.best_kind), static_cast<unsigned>(r.best_window), r.best_horizon,
              static_cast<int>(r.best_sign), static_cast<long long>(r.best_threshold_ppm),
              static_cast<long long>(r.best_n), static_cast<long long>(r.best_ic),
              static_cast<long long>(r.best_rank_ic), static_cast<long long>(r.best_hit),
              static_cast<long long>(r.best_turnover), static_cast<long long>(r.best_score));
        for (int i = 0; i < r.rows_n; ++i) {
            const trading_jobs::ResearchRowOut& d = r.rows[i];
            if (i) o.add(",");
            o.add("{\"kind\":%d,\"window\":%d,\"horizon\":%d,\"n\":%lld,\"ic\":%lld,\"rank_ic\":%lld,"
                  "\"hit\":%lld,\"turnover\":%lld,\"icir\":%lld,\"score\":%lld}",
                  d.kind, d.window, d.horizon, static_cast<long long>(d.n), static_cast<long long>(d.ic),
                  static_cast<long long>(d.rank_ic), static_cast<long long>(d.hit),
                  static_cast<long long>(d.turnover), static_cast<long long>(d.icir),
                  static_cast<long long>(d.score));
        }
        o.add("]}");
        json_len_ = o.n;
    }

    void render_backtest() noexcept {
        trading_jobs::BacktestResult r{};
        const bool have = jobs_.backtest(r);
        const uint64_t req = jobs_.backtest_req_id();
        if (req != 0 && (!have || r.id != req)) {
            std::snprintf(json_, kJsonCap,
                          "{\"code\":0,\"id\":%llu,\"state\":\"queued\",\"started_us\":0,\"done_us\":0,"
                          "\"note\":\"\",\"kind\":0,\"window\":0,\"sign\":1,\"threshold_ppm\":0,\"qty\":0,"
                          "\"fee_ppm\":0,\"cash\":0,\"points\":0,\"trades\":0,\"round_trips\":0,\"wins\":0,"
                          "\"gross_pnl\":0,\"fees\":0,\"net_pnl\":0,\"turnover\":0,\"return_ppm\":0,"
                          "\"annual_ppm\":0,\"sharpe_ppm\":0,\"max_dd_ppm\":0,\"win_ppm\":0,"
                          "\"exposure_ppm\":0,\"final_equity\":0,\"span_us\":0,\"curve_n\":0,\"curve\":[]}",
                          static_cast<unsigned long long>(req));
            json_len_ = static_cast<int>(std::strlen(json_));
            return;
        }
        if (!have) {
            static const char kIdle[] = "{\"code\":0,\"state\":\"idle\",\"id\":0,\"curve_n\":0,\"curve\":[]}";
            std::snprintf(json_, kJsonCap, "%s", kIdle);
            json_len_ = static_cast<int>(std::strlen(json_));
            return;
        }
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"id\":%llu,\"state\":\"%s\",\"started_us\":%lld,\"done_us\":%lld,"
              "\"note\":\"%s\",\"kind\":%u,\"window\":%u,\"sign\":%d,\"threshold_ppm\":%lld,\"qty\":%lld,"
              "\"fee_ppm\":%lld,\"cash\":%lld,\"points\":%lld,\"trades\":%lld,\"round_trips\":%lld,"
              "\"wins\":%lld,\"gross_pnl\":%lld,\"fees\":%lld,\"net_pnl\":%lld,\"turnover\":%lld,"
              "\"return_ppm\":%lld,\"annual_ppm\":%lld,\"sharpe_ppm\":%lld,\"max_dd_ppm\":%lld,"
              "\"win_ppm\":%lld,\"exposure_ppm\":%lld,\"final_equity\":%lld,\"span_us\":%lld,"
              "\"curve_n\":%d,\"curve\":[",
              static_cast<unsigned long long>(r.id), trading_jobs::job_state_name(r.state),
              static_cast<long long>(r.started_us), static_cast<long long>(r.done_us), r.note,
              static_cast<unsigned>(r.kind), static_cast<unsigned>(r.window), static_cast<int>(r.sign),
              static_cast<long long>(r.threshold_ppm), static_cast<long long>(r.qty),
              static_cast<long long>(r.fee_ppm), static_cast<long long>(r.cash),
              static_cast<long long>(r.points), static_cast<long long>(r.trades),
              static_cast<long long>(r.round_trips), static_cast<long long>(r.wins),
              static_cast<long long>(r.gross_pnl), static_cast<long long>(r.fees),
              static_cast<long long>(r.net_pnl), static_cast<long long>(r.turnover),
              static_cast<long long>(r.return_ppm), static_cast<long long>(r.annual_ppm),
              static_cast<long long>(r.sharpe_ppm), static_cast<long long>(r.max_dd_ppm),
              static_cast<long long>(r.win_ppm), static_cast<long long>(r.exposure_ppm),
              static_cast<long long>(r.final_equity), static_cast<long long>(r.span_us), r.curve_n);
        for (int i = 0; i < r.curve_n; ++i) {
            if (i) o.add(",");
            o.add("{\"t\":%lld,\"eq\":%lld,\"dd\":%lld}", static_cast<long long>(r.curve[i].ts_us),
                  static_cast<long long>(r.curve[i].equity), static_cast<long long>(r.curve[i].dd_ppm));
        }
        o.add("]}");
        json_len_ = o.n;
    }

    // Tape endpoints accept only relative paths so a query cannot escape the
    // working directory.
    static bool tape_path_ok(const char* p) noexcept {
        if (!p || !p[0] || p[0] == '/') return false;
        return std::strstr(p, "..") == nullptr;
    }

    bool tape_missing(pitlane::HttpResp& resp) noexcept {
        static const char kMissing[] = "{\"code\":1,\"msg\":\"missing tape\"}";
        resp.json(404, kMissing, sizeof kMissing - 1);
        return true;
    }

    bool tape_meta(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        char path[512]{};
        if (!trading_ops::query_str(req.query, "path", path, sizeof path) || !tape_path_ok(path)) {
            return bad(resp, "path");
        }
        if (::access(path, R_OK) != 0) return tape_missing(resp);
        if (pitlane::tape_detail::is_block_tape(path)) {
            pitlane::BlockTapeReader r;
            pitlane::TapeMeta m{};
            if (!r.open(path) || !r.meta(m)) return tape_missing(resp);
            Out o{json_, kJsonCap, 0};
            o.add("{\"code\":0,\"path\":\"%s\",\"kind\":\"block\",\"versions\":%u,\"blocks\":%u,\"symbol\":%u,"
                  "\"rows\":%llu,\"bytes\":%llu,\"first_ts\":%lld,\"last_ts\":%lld,\"last_version\":%u}",
                  path, static_cast<unsigned>(m.versions), static_cast<unsigned>(m.blocks),
                  static_cast<unsigned>(m.symbol), static_cast<unsigned long long>(m.rows),
                  static_cast<unsigned long long>(m.bytes), static_cast<long long>(m.first_ts),
                  static_cast<long long>(m.last_ts), static_cast<unsigned>(m.versions));
            resp.json(200, json_, static_cast<uint32_t>(o.n));
            return true;
        }
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"path\":\"%s\",\"kind\":\"csv\",\"rows\":0,\"versions\":0}", path);
        resp.json(200, json_, static_cast<uint32_t>(o.n));
        return true;
    }

    bool tape_rows(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        char path[512]{};
        if (!trading_ops::query_str(req.query, "path", path, sizeof path) || !tape_path_ok(path)) {
            return bad(resp, "path");
        }
        int64_t version = 0, since = INT64_MIN, until = INT64_MAX, offset = 0, limit = 50, cols = 255;
        (void)trading_ops::query_i64(req.query, "version", version);
        (void)trading_ops::query_i64(req.query, "since", since);
        (void)trading_ops::query_i64(req.query, "until", until);
        (void)trading_ops::query_i64(req.query, "offset", offset);
        (void)trading_ops::query_i64(req.query, "limit", limit);
        (void)trading_ops::query_i64(req.query, "cols", cols);
        if (offset < 0) offset = 0;
        if (offset > 0x7fffffffll) offset = 0x7fffffffll;
        if (limit <= 0) limit = 50;
        if (limit > 500) limit = 500; // keeps a full page well under kJsonCap
        if (cols < 1) cols = 1;
        if (cols > 255) cols = 255;
        if (since > until) return bad(resp, "since/until");
        if (::access(path, R_OK) != 0) return tape_missing(resp);
        if (!pitlane::tape_detail::is_block_tape(path)) return bad(resp, "block tape");

        pitlane::BlockTapeReader r;
        pitlane::TapeMeta m{};
        if (!r.open(path) || !r.meta(m)) return tape_missing(resp);
        uint32_t want_v = 0;
        if (version > 0) {
            if (version > static_cast<int64_t>(m.versions)) return bad(resp, "version");
            want_v = static_cast<uint32_t>(version);
        }
        const uint32_t shown_v = want_v ? want_v : m.versions;
        const uint8_t colbits = static_cast<uint8_t>(cols);
        pitlane::TapeTick ticks[500]{};
        uint32_t n = 0;
        if (!r.rows(want_v, since, until, static_cast<uint32_t>(offset), static_cast<uint32_t>(limit), colbits, ticks,
                    static_cast<uint32_t>(limit), n)) {
            return bad(resp, "version");
        }
        const uint64_t total = r.rows_of(want_v);

        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"version\":%u,\"rows\":%u,\"total\":%llu,\"ticks\":[", static_cast<unsigned>(shown_v),
              static_cast<unsigned>(n), static_cast<unsigned long long>(total));
        for (uint32_t i = 0; i < n; ++i) {
            const pitlane::TapeTick& k = ticks[i];
            if (i) o.add(",");
            o.add("{");
            const char* sep = "";
            if (colbits & pitlane::kTapeColExch) {
                o.add("\"ts\":%lld", static_cast<long long>(k.exch_us));
                sep = ",";
            }
            if (colbits & pitlane::kTapeColBid) {
                o.add("%s\"bid\":%lld", sep, static_cast<long long>(k.bid_px));
                sep = ",";
            }
            if (colbits & pitlane::kTapeColAsk) {
                o.add("%s\"ask\":%lld", sep, static_cast<long long>(k.ask_px));
                sep = ",";
            }
            if (colbits & pitlane::kTapeColLast) {
                o.add("%s\"last\":%lld", sep, static_cast<long long>(k.last_px));
                sep = ",";
            }
            if (colbits & pitlane::kTapeColBidQty) {
                o.add("%s\"bid_qty\":%lld", sep, static_cast<long long>(k.bid_qty));
                sep = ",";
            }
            if (colbits & pitlane::kTapeColAskQty) {
                o.add("%s\"ask_qty\":%lld", sep, static_cast<long long>(k.ask_qty));
                sep = ",";
            }
            o.add("}");
        }
        o.add("]}");
        resp.json(200, json_, static_cast<uint32_t>(o.n));
        return true;
    }

    bool tape_describe(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        char path[512]{};
        if (!trading_ops::query_str(req.query, "path", path, sizeof path) || !tape_path_ok(path)) {
            return bad(resp, "path");
        }
        int64_t version = 0;
        (void)trading_ops::query_i64(req.query, "version", version);
        if (::access(path, R_OK) != 0) return tape_missing(resp);
        if (!pitlane::tape_detail::is_block_tape(path)) return bad(resp, "block tape");

        pitlane::BlockTapeReader r;
        pitlane::TapeMeta m{};
        if (!r.open(path) || !r.meta(m)) return tape_missing(resp);
        uint32_t want_v = 0;
        if (version > 0) {
            if (version > static_cast<int64_t>(m.versions)) return bad(resp, "version");
            want_v = static_cast<uint32_t>(version);
        }
        pitlane::TapeStats st{};
        if (!r.describe(want_v, st)) return bad(resp, "version");
        static const char* kNames[pitlane::kTapeColumns] = {"exch_us", "local_us", "bid_px", "ask_px",
                                                            "last_px", "bid_qty",  "ask_qty", "last_qty"};
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"version\":%u,\"rows\":%llu,\"cols\":[", static_cast<unsigned>(st.version),
              static_cast<unsigned long long>(st.rows));
        for (uint32_t c = 0; c < pitlane::kTapeColumns; ++c) {
            if (c) o.add(",");
            const pitlane::TapeColStats& s = st.col[c];
            o.add("{\"name\":\"%s\",\"min\":%lld,\"max\":%lld,\"last\":%lld,\"mean\":%.6f}", kNames[c],
                  static_cast<long long>(s.min), static_cast<long long>(s.max), static_cast<long long>(s.last),
                  s.mean);
        }
        o.add("]}");
        resp.json(200, json_, static_cast<uint32_t>(o.n));
        return true;
    }

    void render_strategies(uint8_t env) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"strategies\":[");
        bool first = true;
        for (int i = 0; i < pitlane::StrategyRegistry::kMaxStrategies; ++i) {
            pitlane::StratRow r{};
            if (!strategies_.read(i, r) || !r.id[0] || r.env != env) continue;
            if (!first) o.add(",");
            first = false;
            o.add("{\"id\":\"%s\",\"state\":%u,\"state_name\":\"%s\",\"kind\":%u,\"window\":%u,"
                  "\"sign\":%d,\"acct\":%u,\"instrument\":%u,\"instrument_b\":%u,\"ratio_ppm\":%lld,"
                  "\"threshold_ppm\":%lld,\"qty\":%lld,\"factor\":%lld,\"position\":%lld,"
                  "\"signals\":%lld,\"orders\":%lld,\"rejects\":%lld,"
                  "\"realized\":%lld,\"unrealized\":%lld,\"shared\":%u,\"last_sig_us\":%lld,"
                  "\"started_us\":%lld,\"note\":\"%s\"}",
                  r.id, static_cast<unsigned>(r.state), pitlane::strat_state_name(r.state),
                  static_cast<unsigned>(r.kind), static_cast<unsigned>(r.window), static_cast<int>(r.sign),
                  static_cast<unsigned>(r.acct), static_cast<unsigned>(r.instrument),
                  static_cast<unsigned>(r.instrument_b), static_cast<long long>(r.ratio_ppm),
                  static_cast<long long>(r.threshold_ppm), static_cast<long long>(r.qty),
                  static_cast<long long>(r.factor), static_cast<long long>(r.position),
                  static_cast<long long>(r.signals), static_cast<long long>(r.orders),
                  static_cast<long long>(r.rejects), static_cast<long long>(r.realized),
                  static_cast<long long>(r.unrealized), static_cast<unsigned>(r.shared),
                  static_cast<long long>(r.last_sig_us), static_cast<long long>(r.started_us), r.note);
        }
        o.add("]}");
        json_len_ = o.n;
    }

    // 配对价差的实时视图：只报当前环境的 kind=9 实例。
    void render_pair(uint8_t env) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"pairs\":[");
        bool first = true;
        int n = 0;
        for (int i = 0; i < pitlane::StrategyRegistry::kMaxStrategies; ++i) {
            pitlane::StratRow r{};
            if (!strategies_.read(i, r) || !r.id[0] || r.env != env) continue;
            if (r.kind != pitlane::kStratPairKind) continue;
            if (!first) o.add(",");
            first = false;
            ++n;
            o.add("{\"id\":\"%s\",\"state\":%u,\"state_name\":\"%s\",\"name_a\":%u,\"name_b\":%u,"
                  "\"ratio_ppm\":%lld,\"spread\":%lld,\"mean\":%lld,\"sd\":%lld,\"z_ppm\":%lld,"
                  "\"pos_a\":%lld,\"pos_b\":%lld,\"qty\":%lld,\"rounds\":%lld,\"wins\":%lld,"
                  "\"losses\":%lld,\"realized\":%lld,\"unrealized\":%lld,\"signals\":%lld,"
                  "\"orders\":%lld,\"rejects\":%lld,\"timeouts\":%lld,\"stop_hits\":%lld,\"ready\":%u}",
                  r.id, static_cast<unsigned>(r.state), pitlane::pair_state_name(r.state),
                  static_cast<unsigned>(r.instrument), static_cast<unsigned>(r.instrument_b),
                  static_cast<long long>(r.ratio_ppm), static_cast<long long>(r.spread),
                  static_cast<long long>(r.mean), static_cast<long long>(r.sd),
                  static_cast<long long>(r.factor), static_cast<long long>(r.position),
                  static_cast<long long>(r.position_b), static_cast<long long>(r.qty),
                  static_cast<long long>(r.rounds), static_cast<long long>(r.wins),
                  static_cast<long long>(r.losses), static_cast<long long>(r.realized),
                  static_cast<long long>(r.unrealized), static_cast<long long>(r.signals),
                  static_cast<long long>(r.orders), static_cast<long long>(r.rejects),
                  static_cast<long long>(r.timeouts), static_cast<long long>(r.stop_hits),
                  static_cast<unsigned>(r.pair_ready));
        }
        o.add("],\"n\":%d,\"packages\":[", n);
        pitlane::PairPackage pk[kPkgCap]{};
        const int pn = packages_.list(pk, kPkgCap);
        for (int i = 0; i < (pn > 0 ? pn : 0); ++i) {
            const pitlane::PairPackage& r = pk[i];
            if (i) o.add(",");
            o.add("{\"id\":%u,\"state\":%u,\"state_name\":\"%s\",\"name_a\":%u,\"name_b\":%u,"
                  "\"ratio_ppm\":%lld,\"qty_a\":%lld,\"qty_b\":%lld,\"filled_a\":%lld,\"filled_b\":%lld,"
                  "\"remain_a\":%lld,\"remain_b\":%lld,\"fx_ppm\":%lld,\"note\":\"%s\"}",
                  r.id, static_cast<unsigned>(r.state), pitlane::pair_package_state_name(r.state), r.name_a, r.name_b,
                  static_cast<long long>(r.ratio_ppm), static_cast<long long>(r.qty_a),
                  static_cast<long long>(r.qty_b), static_cast<long long>(r.filled_a),
                  static_cast<long long>(r.filled_b), static_cast<long long>(r.qty_a - r.filled_a),
                  static_cast<long long>(r.qty_b - r.filled_b), static_cast<long long>(r.fx_ppm), r.note);
        }
        o.add("],\"pkg_n\":%d}", pn > 0 ? pn : 0);
        json_len_ = o.n;
    }

    // POST /api/pair: create, update, cancel, close, or fill a manual package.
    bool post_pair(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        char action[16]{};
        if (!trading_ops::key_str(req.body, req.body_len, "action", action, sizeof action)) {
            return bad(resp, "action");
        }
        int64_t id = 0;
        int64_t qty = 0;
        int64_t leg = 0;
        int64_t qty_a = 0;
        int64_t ratio = 1000000;
        int64_t fx = 1000000;
        int64_t name_a = 0;
        int64_t name_b = 0;
        char note[24]{};
        const bool has_qty_a = trading_ops::key_i64(req.body, req.body_len, "qty_a", qty_a);
        const bool has_ratio = trading_ops::key_i64(req.body, req.body_len, "ratio_ppm", ratio);
        const bool has_fx = trading_ops::key_i64(req.body, req.body_len, "fx_ppm", fx);
        const bool has_note = trading_ops::key_str(req.body, req.body_len, "note", note, sizeof note);
        (void)trading_ops::key_i64(req.body, req.body_len, "name_a", name_a);
        (void)trading_ops::key_i64(req.body, req.body_len, "name_b", name_b);
        (void)trading_ops::key_i64(req.body, req.body_len, "qty", qty);
        (void)trading_ops::key_i64(req.body, req.body_len, "leg", leg);

        if (std::strcmp(action, "create") == 0) {
            pitlane::PairPackageCfg cfg{};
            cfg.name_a = static_cast<uint32_t>(name_a);
            cfg.name_b = static_cast<uint32_t>(name_b);
            cfg.qty_a = has_qty_a ? qty_a : qty;
            cfg.ratio_ppm = ratio;
            cfg.fx_ppm = fx;
            if (has_note) pitlane::pair_package_note(cfg.note, sizeof cfg.note, note);
            const uint32_t nid = packages_.create(cfg);
            if (nid == 0) return fail(resp, "name_a, name_b and qty_a must be valid; the book may be full");
            save_packages();
            log_pair("create", nid);
            return pair_ok(resp, nid);
        }
        if (!trading_ops::key_i64(req.body, req.body_len, "id", id) || id <= 0) return bad(resp, "id");
        const uint32_t pid = static_cast<uint32_t>(id);
        if (std::strcmp(action, "update") == 0) {
            pitlane::PairPackage cur{};
            if (!packages_.get(pid, cur)) return fail(resp, "package not found");
            pitlane::PairPackageCfg cfg{};
            cfg.name_a = cur.name_a;
            cfg.name_b = cur.name_b;
            cfg.qty_a = has_qty_a ? qty_a : cur.qty_a;
            cfg.ratio_ppm = has_ratio ? ratio : cur.ratio_ppm;
            cfg.fx_ppm = has_fx ? fx : cur.fx_ppm;
            pitlane::pair_package_note(cfg.note, sizeof cfg.note, cur.note);
            if (has_note) pitlane::pair_package_note(cfg.note, sizeof cfg.note, note);
            if (!packages_.update(pid, cfg)) return fail(resp, "update refused: closed package or target below filled");
            save_packages();
            log_pair("update", pid);
            return pair_ok(resp, pid);
        }
        if (std::strcmp(action, "cancel") == 0) {
            if (!packages_.cancel(pid)) return fail(resp, "cancel refused: not an open package");
            save_packages();
            log_pair("cancel", pid);
            return pair_ok(resp, pid);
        }
        if (std::strcmp(action, "close") == 0) {
            if (!packages_.close(pid)) return fail(resp, "package is not open or legs are incomplete");
            save_packages();
            log_pair("close", pid);
            return pair_ok(resp, pid);
        }
        if (std::strcmp(action, "fill") == 0) {
            if (!packages_.fill(pid, static_cast<int>(leg), qty)) return fail(resp, "leg, qty, or overfill");
            save_packages();
            log_pair("fill", pid);
            return pair_ok(resp, pid);
        }
        return fail(resp, "unknown action");
    }

    bool pair_ok(pitlane::HttpResp& resp, uint32_t id) noexcept {
        pitlane::PairPackage p{};
        (void)packages_.get(id, p);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"msg\":\"ok\",\"id\":%u,\"state\":\"%s\"}", id, pitlane::pair_package_state_name(p.state));
        resp.json(200, json_, static_cast<uint32_t>(o.n));
        return true;
    }

    void log_pair(const char* what, uint32_t id) noexcept {
        char line[pitlane::LogIndex::kText];
        std::snprintf(line, sizeof line, "pair package %s id=%u", what, id);
        logs_.add(now_us(), pitlane::kLogInfo, pitlane::kLogOps, line);
    }

    // GET /api/credit: per-account credit terms and short inventory.
    void render_credit(uint8_t env) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"accounts\":[");
        bool first = true;
        for (int i = 0; i < accs_.count() && i < 8; ++i) {
            const pitlane::AccountSpec& a = accs_.spec(i);
            if (a.env != env) continue;
            pitlane::CreditTerms t{};
            pitlane::CreditCash c{};
            (void)credit_.get(a.num, t);
            (void)credit_.get_cash(a.num, c);
            int64_t long_qty = 0;
            int64_t short_qty = 0;
            pitlane::ClearingReport rep{};
            if (clear_board_[i].read(rep)) {
                long_qty = rep.sum_long();
                short_qty = rep.sum_short();
            }
            if (!first) o.add(",");
            first = false;
            o.add("{\"acct\":%u,\"id\":\"%s\",\"class\":\"%s\",\"ccy\":\"%s\",\"source\":%u,"
                  "\"asset_balance\":%lld,\"total_debt\":%lld,\"fund_debt\":%lld,\"stock_debt\":%lld,"
                  "\"assure_ratio_ppm\":%lld,\"finance_quota\":%lld,\"finance_left\":%lld,"
                  "\"shortsell_quota\":%lld,\"shortsell_left\":%lld,"
                  "\"long_qty\":%lld,\"short_qty\":%lld,\"special_short\":%lld}",
                  a.num, a.id, a.klass, a.ccy, static_cast<unsigned>(t.source),
                  static_cast<long long>(t.asset_balance), static_cast<long long>(t.total_debt),
                  static_cast<long long>(t.fund_debt), static_cast<long long>(t.stock_debt),
                  static_cast<long long>(pitlane::credit_assure_ppm(t)),
                  static_cast<long long>(t.finance_quota),
                  static_cast<long long>(pitlane::credit_finance_left(t, t.fund_debt)),
                  static_cast<long long>(t.shortsell_quota),
                  static_cast<long long>(pitlane::credit_shortsell_left(t, t.stock_debt)),
                  static_cast<long long>(long_qty), static_cast<long long>(short_qty + c.short_qty),
                  static_cast<long long>(c.special_short));
        }
        o.add("]}");
        json_len_ = o.n;
    }

    // POST /api/credit: set or clear one account's terms. Until a credit
    // adapter exists this is how an operator or back office feeds the console.
    bool post_credit(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        char action[16]{};
        if (!trading_ops::key_str(req.body, req.body_len, "action", action, sizeof action)) {
            return bad(resp, "action");
        }
        int64_t acct = 0;
        if (!trading_ops::key_i64(req.body, req.body_len, "acct", acct) || acct <= 0) return bad(resp, "acct");
        const uint32_t num = static_cast<uint32_t>(acct);
        if (std::strcmp(action, "clear") == 0) {
            if (!credit_.erase(num)) return fail(resp, "account has no credit terms");
            save_credit();
            return ok_json(resp);
        }
        if (std::strcmp(action, "set") != 0) return fail(resp, "unknown action");
        pitlane::CreditTerms t{};
        if (!credit_.get(num, t)) t = pitlane::CreditTerms{};
        (void)trading_ops::key_i64(req.body, req.body_len, "asset_balance", t.asset_balance);
        (void)trading_ops::key_i64(req.body, req.body_len, "total_debt", t.total_debt);
        (void)trading_ops::key_i64(req.body, req.body_len, "fund_debt", t.fund_debt);
        (void)trading_ops::key_i64(req.body, req.body_len, "stock_debt", t.stock_debt);
        (void)trading_ops::key_i64(req.body, req.body_len, "assure_ratio_ppm", t.assure_ratio_ppm);
        (void)trading_ops::key_i64(req.body, req.body_len, "finance_quota", t.finance_quota);
        (void)trading_ops::key_i64(req.body, req.body_len, "shortsell_quota", t.shortsell_quota);
        t.source = pitlane::kCreditSourceManual;
        if (!credit_.set(num, t)) return fail(resp, "negative value");
        save_credit();
        return ok_json(resp);
    }

    bool ok_json(pitlane::HttpResp& resp) noexcept {
        static const char kOk[] = "{\"code\":0,\"msg\":\"ok\"}";
        resp.json(200, kOk, sizeof kOk - 1);
        return true;
    }

    bool busy(pitlane::HttpResp& resp) noexcept {
        static const char kBusy[] = "{\"code\":5,\"msg\":\"job queue busy\"}";
        resp.json(503, kBusy, sizeof kBusy - 1);
        return true;
    }

    bool post_research(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        trading_jobs::JobReq jr{};
        jr.kind = 1;
        (void)trading_ops::key_str(req.body, req.body_len, "tape", jr.tape, sizeof jr.tape);
        int64_t name = 0;
        if (trading_ops::key_i64(req.body, req.body_len, "name", name)) jr.name = static_cast<uint32_t>(name);
        if (!jr.tape[0]) std::snprintf(jr.tape, sizeof jr.tape, "%.*s", static_cast<int>(sizeof jr.tape) - 1, tape_path_);
        if (!jobs_.submit(jr)) return busy(resp);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"msg\":\"queued\",\"id\":%llu,\"state\":\"queued\"}",
              static_cast<unsigned long long>(jr.id));
        json_len_ = o.n;
        resp.json(202, json_, static_cast<uint32_t>(json_len_));
        return true;
    }

    bool post_backtest(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        trading_jobs::JobReq jr{};
        jr.kind = 2;
        (void)trading_ops::key_str(req.body, req.body_len, "tape", jr.tape, sizeof jr.tape);
        int64_t v = 0;
        if (trading_ops::key_i64(req.body, req.body_len, "name", v)) jr.name = static_cast<uint32_t>(v);
        if (trading_ops::key_i64(req.body, req.body_len, "kind", v)) jr.factor_kind = static_cast<uint8_t>(v);
        if (trading_ops::key_i64(req.body, req.body_len, "window", v)) jr.window = static_cast<uint16_t>(v);
        if (trading_ops::key_i64(req.body, req.body_len, "sign", v)) jr.sign = static_cast<int8_t>(v);
        if (trading_ops::key_i64(req.body, req.body_len, "threshold_ppm", v)) jr.threshold_ppm = v;
        if (trading_ops::key_i64(req.body, req.body_len, "qty", v)) jr.qty = v;
        if (trading_ops::key_i64(req.body, req.body_len, "fee_ppm", v)) jr.fee_ppm = v;
        if (trading_ops::key_i64(req.body, req.body_len, "cash", v)) jr.cash = v;
        if (!jr.tape[0]) std::snprintf(jr.tape, sizeof jr.tape, "%.*s", static_cast<int>(sizeof jr.tape) - 1, tape_path_);
        if (jr.factor_kind < 1 || jr.factor_kind > 8 || jr.window < 2 || jr.window > 256 ||
            (jr.sign != 1 && jr.sign != -1) || jr.threshold_ppm <= 0 || jr.qty <= 0 || jr.cash <= 0) {
            return bad(resp, "params");
        }
        if (!jobs_.submit(jr)) return busy(resp);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"msg\":\"queued\",\"id\":%llu,\"state\":\"queued\"}",
              static_cast<unsigned long long>(jr.id));
        json_len_ = o.n;
        resp.json(202, json_, static_cast<uint32_t>(json_len_));
        return true;
    }

    bool post_strategy(const pitlane::HttpReq& req, pitlane::HttpResp& resp, uint8_t env) noexcept {
        char action[16]{};
        char id[24]{};
        if (!trading_ops::key_str(req.body, req.body_len, "action", action, sizeof action) ||
            !trading_ops::key_str(req.body, req.body_len, "id", id, sizeof id)) {
            return bad(resp, "action/id");
        }
        if (!id[0]) return bad(resp, "id");
        pitlane::StratCmd c{};
        c.env = env;
        std::snprintf(c.id, sizeof c.id, "%s", id);
        if (std::strcmp(action, "add") == 0) {
            char acct_tok[24]{};
            int64_t instrument = 0;
            int64_t kind = 0;
            int64_t window = 0;
            int64_t sign = 1;
            int64_t thr = 0;
            int64_t qty = 0;
            int64_t cd_ms = 0;
            int64_t instrument_b = 0;
            int64_t ratio_ppm = 1000000;
            if (!trading_ops::key_i64(req.body, req.body_len, "instrument", instrument) ||
                !trading_ops::key_i64(req.body, req.body_len, "kind", kind) ||
                !trading_ops::key_i64(req.body, req.body_len, "window", window) ||
                !trading_ops::key_i64(req.body, req.body_len, "sign", sign) ||
                !trading_ops::key_i64(req.body, req.body_len, "threshold_ppm", thr) ||
                !trading_ops::key_i64(req.body, req.body_len, "qty", qty) ||
                !trading_ops::key_i64(req.body, req.body_len, "cooldown_ms", cd_ms)) {
                return bad(resp, "instrument/kind/window/sign/threshold_ppm/qty/cooldown_ms");
            }
            (void)trading_ops::key_str(req.body, req.body_len, "acct", acct_tok, sizeof acct_tok);
            uint32_t acct = acct_tok[0] ? accs_.resolve(acct_tok) : 0;
            if (acct == 0) {
                for (int i = 0; i < accs_.count(); ++i) {
                    if (accs_.spec(i).env == env) {
                        acct = accs_.spec(i).num;
                        break;
                    }
                }
            }
            if (acct == 0) return bad(resp, "acct");
            (void)trading_ops::key_i64(req.body, req.body_len, "instrument_b", instrument_b);
            (void)trading_ops::key_i64(req.body, req.body_len, "ratio_ppm", ratio_ppm);
            if (instrument <= 0 || kind < 1 || kind > pitlane::kStratPairKind || window < 2 ||
                window > 256 || (sign != 1 && sign != -1) || thr <= 0 || qty <= 0 || cd_ms < 0) {
                return bad(resp, "params");
            }
            if (kind == pitlane::kStratPairKind) {
                if (instrument_b <= 0 || instrument_b == instrument || ratio_ppm <= 0) {
                    return bad(resp, "instrument_b/ratio_ppm");
                }
            }
            c.kind = pitlane::kStratCmdAdd;
            std::snprintf(c.spec.id, sizeof c.spec.id, "%s", id);
            c.spec.acct = acct;
            c.spec.instrument = static_cast<uint32_t>(instrument);
            c.spec.instrument_b = static_cast<uint32_t>(instrument_b);
            c.spec.ratio_ppm = ratio_ppm;
            c.spec.kind = static_cast<uint8_t>(kind);
            c.spec.window = static_cast<uint16_t>(window);
            c.spec.sign = static_cast<int8_t>(sign);
            c.spec.threshold_ppm = thr;
            c.spec.qty = qty;
            c.spec.cooldown_us = cd_ms * 1000;
            c.spec.env = env;
        } else if (std::strcmp(action, "remove") == 0) {
            c.kind = pitlane::kStratCmdRemove;
        } else if (std::strcmp(action, "start") == 0) {
            c.kind = pitlane::kStratCmdStart;
        } else if (std::strcmp(action, "stop") == 0) {
            c.kind = pitlane::kStratCmdStop;
        } else if (std::strcmp(action, "pause") == 0) {
            c.kind = pitlane::kStratCmdPause;
        } else if (std::strcmp(action, "resume") == 0) {
            c.kind = pitlane::kStratCmdResume;
        } else {
            return bad(resp, "action");
        }
        if (!strategies_.submit(c)) return busy(resp);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"msg\":\"queued\"}");
        json_len_ = o.n;
        resp.json(202, json_, static_cast<uint32_t>(json_len_));
        return true;
    }

    bool apply_research(pitlane::HttpResp& resp) noexcept {
        trading_jobs::ResearchResult r{};
        if (!jobs_.research(r) || r.state != trading_jobs::kJobDone || r.rows_n <= 0) {
            return bad(resp, "no research result");
        }
        const uint64_t req = jobs_.research_req_id();
        if (req != 0 && r.id != req) return bad(resp, "research still running");
        char path[640]{};
        trading_ops::conf_sibling(conf_path_, "research.conf", path, sizeof path);
        pitlane::StudyCfg cfg{};
        cfg.kind = r.best_kind;
        cfg.window = r.best_window;
        cfg.sign = r.best_sign;
        cfg.signal_ppm = r.best_threshold_ppm > 0 ? r.best_threshold_ppm : 1;
        cfg.horizon_bars = r.best_horizon > 0 ? r.best_horizon : 1;
        cfg.samples = r.best_n;
        cfg.ic_ppm = r.best_ic;
        cfg.rank_ic_ppm = r.best_rank_ic;
        cfg.hit_rate_ppm = r.best_hit;
        cfg.turnover_ppm = r.best_turnover;
        cfg.score_ppm = r.best_score;
        if (!pitlane::study_conf_save(path, cfg)) return bad(resp, "cannot write research.conf");
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"msg\":\"applied\",\"path\":\"%s\",\"kind\":%u,\"window\":%u,\"sign\":%d,"
              "\"threshold_ppm\":%lld,\"horizon\":%d}",
              path, static_cast<unsigned>(cfg.kind), static_cast<unsigned>(cfg.window),
              static_cast<int>(cfg.sign), static_cast<long long>(cfg.signal_ppm), cfg.horizon_bars);
        json_len_ = o.n;
        resp.json(200, json_, static_cast<uint32_t>(json_len_));
        return true;
    }

    bool allow(const pitlane::OpsSession& s, uint32_t need, pitlane::HttpResp& resp) noexcept {
        if (pitlane::ops_has(s.perms, need)) return true;
        static const char kForbidden[] = "{\"code\":7,\"msg\":\"forbidden\"}";
        resp.json(403, kForbidden, sizeof kForbidden - 1);
        return false;
    }

    bool unauthorized(pitlane::HttpResp& resp) noexcept {
        static const char kNoAuth[] = "{\"code\":6,\"msg\":\"login required\"}";
        resp.json(401, kNoAuth, sizeof kNoAuth - 1);
        resp.header("WWW-Authenticate: Bearer realm=\"pitlane\"\r\n");
        return true;
    }

    bool method_denied(pitlane::HttpResp& resp) noexcept {
        static const char kDeny[] = "{\"code\":1,\"msg\":\"method not allowed\"}";
        resp.json(405, kDeny, sizeof kDeny - 1);
        return true;
    }

    bool do_login(const pitlane::HttpReq& req, pitlane::HttpResp& resp, int64_t now) noexcept {
        char user[32]{}, pass[64]{};
        if (!trading_ops::key_str(req.body, req.body_len, "user", user, sizeof user) ||
            !trading_ops::key_str(req.body, req.body_len, "password", pass, sizeof pass)) {
            return bad(resp, "user/password");
        }
        uint8_t role = 0;
        uint32_t perms = 0;
        if (!users_.verify(user, pass, role, perms)) {
            static const char kBad[] = "{\"code\":6,\"msg\":\"bad credentials\"}";
            resp.json(401, kBad, sizeof kBad - 1);
            return true;
        }
        std::memset(pass, 0, sizeof pass);
        char mode_tok[8]{};
        uint8_t mode = pitlane::kEnvTest;
        if (trading_ops::key_str(req.body, req.body_len, "mode", mode_tok, sizeof mode_tok)) {
            uint8_t parsed = pitlane::kEnvTest;
            if (pitlane::acc_env_from_name(mode_tok, parsed)) mode = parsed;
            else if (std::strcmp(mode_tok, "1") == 0) mode = pitlane::kEnvLive;
        }
        const int slot = sessions_.create(users_.at(users_.find(user)), now, nullptr, mode);
        if (slot < 0) {
            static const char kBusy[] = "{\"code\":4,\"msg\":\"session limit\"}";
            resp.json(503, kBusy, sizeof kBusy - 1);
            return true;
        }
        const pitlane::OpsSession& s = sessions_.at(slot);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":0,\"msg\":\"ok\",\"user\":\"%s\",\"role\":\"%s\",\"perms\":%u,\"mode\":\"%s\","
              "\"token\":\"%s\"}",
              s.user, pitlane::ops_role_name(s.role), static_cast<unsigned>(s.perms),
              pitlane::acc_env_name(s.mode), s.token);
        resp.header("Set-Cookie: pitlane_token=");
        std::snprintf(resp.extra + std::strlen(resp.extra), sizeof resp.extra - std::strlen(resp.extra),
                      "%s; Path=/; HttpOnly; SameSite=Lax\r\n", s.token);
        json_len_ = o.n;
        resp.json(200, json_, static_cast<uint32_t>(json_len_));
        return true;
    }

    bool do_user(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        char action[16]{}, user[32]{}, pass[64]{}, role[16]{};
        if (!trading_ops::key_str(req.body, req.body_len, "action", action, sizeof action) ||
            !trading_ops::key_str(req.body, req.body_len, "user", user, sizeof user)) {
            return bad(resp, "action/user");
        }
        (void)trading_ops::key_str(req.body, req.body_len, "password", pass, sizeof pass);
        (void)trading_ops::key_str(req.body, req.body_len, "role", role, sizeof role);

        const int idx = users_.find(user);
        const bool target_admin = idx >= 0 && users_.at(idx).role == pitlane::kRoleAdmin && users_.at(idx).enabled;
        int admins = 0;
        for (int i = 0; i < users_.count(); ++i) {
            if (users_.at(i).role == pitlane::kRoleAdmin && users_.at(i).enabled) ++admins;
        }
        const bool removes_admin = target_admin && admins <= 1 &&
                                   (std::strcmp(action, "remove") == 0 || std::strcmp(action, "disable") == 0 ||
                                    std::strcmp(action, "role") == 0);
        if (removes_admin) {
            static const char kLast[] = "{\"code\":1,\"msg\":\"cannot change the last admin\"}";
            resp.json(409, kLast, sizeof kLast - 1);
            return true;
        }

        bool ok = false;
        if (std::strcmp(action, "add") == 0) {
            uint8_t r = 0;
            if (!pitlane::ops_role_from_name(role, r)) return bad(resp, "role");
            ok = users_.add(user, pass, r, true, users_path_);
        } else if (std::strcmp(action, "remove") == 0) {
            ok = users_.remove(user, true, users_path_);
        } else if (std::strcmp(action, "role") == 0) {
            uint8_t r = 0;
            if (!pitlane::ops_role_from_name(role, r)) return bad(resp, "role");
            ok = users_.set_role(user, r, true, users_path_);
        } else if (std::strcmp(action, "password") == 0) {
            ok = users_.set_password(user, pass, true, users_path_);
        } else if (std::strcmp(action, "enable") == 0 || std::strcmp(action, "disable") == 0) {
            ok = users_.set_enabled(user, std::strcmp(action, "enable") == 0, true, users_path_);
        } else {
            return bad(resp, "action");
        }
        if (ok) sessions_.drop_user(user);
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":%d,\"msg\":\"%s\"}", ok ? 0 : 1, ok ? "ok" : "rejected");
        json_len_ = o.n;
        resp.json(ok ? 200 : 400, json_, static_cast<uint32_t>(json_len_));
        return true;
    }

    bool fail(pitlane::HttpResp& resp, const char* msg) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":1,\"msg\":\"%s\"}", msg);
        resp.json(400, json_, static_cast<uint32_t>(o.n));
        return true;
    }

    bool bad(pitlane::HttpResp& resp, const char* key) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"code\":1,\"msg\":\"missing %s\"}", key);
        resp.json(400, json_, static_cast<uint32_t>(o.n));
        return true;
    }

    static bool http_cb(void* user, const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        return static_cast<SimOps*>(user)->handle(req, resp);
    }

    pitlane::OpsUserStore users_{};
    pitlane::OpsSessionStore sessions_{};
    pitlane::SeriesSet series_{};
    pitlane::GatewayRegistry gw_{};
    pitlane::AccountRegistry accs_{};
    pitlane::InstrumentRegistry instr_{};
    pitlane::AlarmBoard alarms_{};
    pitlane::ClearingBook books_[8]{};
    static constexpr int kPkgCap = 16;
    pitlane::PairPackageBook<kPkgCap> packages_{};
    pitlane::CreditBook<8> credit_{};
    pitlane::PreTradeGuard pre_{};
    pitlane::PreLimits pre_limits_{};
    pitlane::ClearingBoard clear_board_[8]{};
    static constexpr int kCashQ = 16;
    CashJob cash_q_[kCashQ]{};
    std::atomic<uint32_t> cash_head_{0};
    std::atomic<uint32_t> cash_tail_{0};
    int64_t cash_req_seq_{0};
    std::atomic<int64_t> cash_ack_{0};
    trading_ops::GwHooks hooks_{};
    char users_path_[512]{};
    char gw_path_[512]{};
    char acc_path_[512]{};
    char instr_path_[512]{};
    char pkg_path_[512]{};
    char credit_path_[512]{};
    char tape_path_[512]{};
    bool gw_on_[pitlane::GatewayRegistry::kMaxGateways]{};
    pitlane::OpsChannel<256> ch_{};
    pitlane::StatusBoard<4> board_{};
    pitlane::RiskGuard guard_{};
    pitlane::HttpServer http_{};
    pitlane::ControlPlane cp_{};
    pitlane::OpsPolicy pol_{};
    pitlane::HostSample host_{};
    pitlane::HealthSample health_{};
    pitlane::HealthState health_st_{};
    pitlane::HealthLimits health_limits_{};
    uint8_t last_health_level_{0};
    std::atomic<int64_t> last_beat_us_{0};
    std::thread sampler_;
    std::atomic<bool> sampler_stop_{false};
    std::atomic<uint64_t> sample_seq_{0};
    SamplerPost sample_post_{};
    uint64_t sample_seen_{0};
    pitlane::FullFlow* flow_{nullptr};
    bool paper_{true};
    int64_t start_us_{0};
    int64_t last_risk_us_{0};
    int64_t last_snap_us_{0};
    int64_t last_watch_us_{0};
    int64_t last_clear_us_{0};
    int64_t last_ledger_us_{0};
    uint64_t clear_seq_{0};
    pitlane::LedgerFeed ledger_{};
    LedgerPollFn ledger_poll_{nullptr};
    void* ledger_user_{nullptr};
    uint8_t watch_sev_[32]{};
    int64_t peak_equity_{kEquityBase};
    int64_t equity_base_{kEquityBase};
    uint64_t snap_seq_{0};
    uint8_t last_risk_code_{0};
    char root_[256]{};
    char bind_[48]{};
    char conf_path_[512]{};
    trading_jobs::JobWorker jobs_{};
    pitlane::StrategyRegistry strategies_{};
    StratRt strat_rt_[pitlane::StrategyRegistry::kMaxStrategies]{};
    char strat_path_[512]{};
    int64_t last_strat_pub_us_{0};
    char* json_{nullptr};
    int json_len_{0};

    // ---- 运维扩展状态 ---------------------------------------------------
    const pitlane::MarketBoard* market_live_{nullptr};
    const pitlane::HopLedger* md_hops_{nullptr};
    pitlane::HopLedger hops_{};
    pitlane::PerfBaseline base_{};
    char base_path_[512]{};
    char baseline_rev_[24]{};
    pitlane::FlameBook flame_{};
    pitlane::SeatRegistry seats_{};
    pitlane::LogIndex logs_{};
    pitlane::Canary canary_{};
    pitlane::CanaryPlan canary_plan_{};
    pitlane::ReplayCtl replay_{};
    pitlane::CycleScale scale_{};
    bool scale_ok_{false};
    int64_t log_alarm_seq_{0};
};

}  // namespace trading_ops
