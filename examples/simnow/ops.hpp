// Ops console adapter for the SimNow example: command channel, status board,
// risk guard, host sampling, and the HTTP handler. The trading thread only
// touches this object through on_beat; the HTTP thread only reads the status
// board and writes commands.

#pragma once

#include "full_flow.hpp"
#include "pitlane/ops/command.hpp"
#include "pitlane/ops/control.hpp"
#include "pitlane/ops/host.hpp"
#include "pitlane/ops/http.hpp"
#include "pitlane/ops/policy.hpp"
#include "pitlane/ops/status.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace simnow_ops {

inline bool key_i64(const char* s, uint32_t n, const char* key, int64_t& out) noexcept {
    if (!s || n == 0) return false;
    char pat[40];
    std::snprintf(pat, sizeof pat, "\"%s\"", key);
    const char* p = std::strstr(s, pat);
    if (!p) return false;
    p += std::strlen(pat);
    const char* end = s + n;
    while (p < end && *p != ':') ++p;
    if (p >= end) return false;
    ++p;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    char* stop = nullptr;
    const long long v = std::strtoll(p, &stop, 10);
    if (stop == p) return false;
    out = v;
    return true;
}

inline bool key_str(const char* s, uint32_t n, const char* key, char* out, int cap) noexcept {
    if (!s || n == 0 || !out || cap <= 0) return false;
    char pat[40];
    std::snprintf(pat, sizeof pat, "\"%s\"", key);
    const char* p = std::strstr(s, pat);
    if (!p) return false;
    p += std::strlen(pat);
    const char* end = s + n;
    while (p < end && *p != ':') ++p;
    if (p >= end) return false;
    ++p;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end || *p != '"') return false;
    ++p;
    int i = 0;
    while (p < end && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return p < end;
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

// Reads `key = value` pairs. Returns false only when the file cannot be opened.
inline bool load_ops_conf(const char* path, pitlane::OpsPolicy& pol, uint16_t& port, char* root, int cap) noexcept {
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
        else if (std::strcmp(key, "max_manual_qty") == 0) pol.max_manual_qty = v;
        else if (std::strcmp(key, "max_manual_notional") == 0) pol.max_manual_notional = v;
        else if (std::strcmp(key, "max_manual_per_min") == 0) pol.max_manual_per_min = static_cast<int32_t>(v);
        else if (std::strcmp(key, "max_daily_loss") == 0) pol.max_daily_loss = v;
        else if (std::strcmp(key, "max_drawdown_ppm") == 0) pol.max_drawdown_ppm = v;
        else if (std::strcmp(key, "max_reject_ppm") == 0) pol.max_reject_ppm = static_cast<int32_t>(v);
        else if (std::strcmp(key, "stale_tick_us") == 0) pol.stale_tick_us = v;
    }
    std::fclose(f);
    return true;
}

class SimOps {
public:
    ~SimOps() { close(); }

    bool open(pitlane::FullFlow& flow, bool paper, uint16_t port, const char* doc_root, const char* ops_conf) noexcept {
        flow_ = &flow;
        paper_ = paper;
        pol_ = pitlane::OpsPolicy{};
        std::snprintf(root_, sizeof root_, "%s", doc_root && doc_root[0] ? doc_root : "web");
        std::snprintf(conf_path_, sizeof conf_path_, "%s", ops_conf ? ops_conf : "run/ops.conf");
        const bool have_conf = simnow_ops::load_ops_conf(conf_path_, pol_, port, root_, sizeof root_);
        if (!have_conf) {
            std::printf("[ops] %s not found, using defaults\n", conf_path_);
        }

        json_ = static_cast<char*>(std::malloc(kJsonCap));
        if (!json_) return false;

        cp_.open(&ch_, &flow.rms(), pol_);
        cp_.set_account(1);
        cp_.set_mint(&flow.mint());
        cp_.set_send(&SimOps::send_cb, this);
        cp_.set_top(&SimOps::top_cb, this);
        cp_.set_net(&SimOps::net_cb, this);
        cp_.set_reload(&SimOps::reload_cb, this);
        cp_.set_cancel(&SimOps::cancel_cb, this);
        flow.set_control(&cp_);
        flow.set_ops(&SimOps::beat_cb, this);
        guard_.set(pol_);
        start_us_ = now_us();
        last_host_us_ = 0;
        last_risk_us_ = 0;
        last_snap_us_ = 0;
        peak_equity_ = kEquityBase;
        flow_ = &flow;

        if (port == 0) {
            std::printf("[ops] web disabled\n");
            return true;
        }
        if (!http_.start(port, &SimOps::http_cb, this, root_)) {
            std::printf("[ops] HTTP bind on %u failed; trading continues without web\n", static_cast<unsigned>(port));
        }
        return true;
    }

    void close() noexcept {
        http_.stop();
        if (json_) {
            std::free(json_);
            json_ = nullptr;
        }
        flow_ = nullptr;
    }

    bool web() const noexcept { return http_.running(); }
    uint16_t port() const noexcept { return http_.port(); }

    pitlane::ControlPlane& control() noexcept { return cp_; }
    const pitlane::OpsPolicy& policy() const noexcept { return pol_; }

private:
    static constexpr int64_t kEquityBase = 1000000000;
    static constexpr int kJsonCap = 32768;

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

    static void top_cb(void* user, uint32_t name, pitlane::ManualQuote& out) noexcept {
        auto* self = static_cast<SimOps*>(user);
        if (self->flow_) self->flow_->last_quote(name, out.bid, out.ask);
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
        if (!simnow_ops::load_ops_conf(self->conf_path_, next, port, root, sizeof root)) return false;
        self->pol_ = next;
        self->guard_.set(next);
        out = next;
        return true;
    }

    static void cancel_cb(void* user) noexcept {
        auto* self = static_cast<SimOps*>(user);
        if (self->flow_) self->flow_->cancel_all();
    }

    static int beat_cb(void* user, int64_t now_us, pitlane::FullFlow& flow) noexcept {
        return static_cast<SimOps*>(user)->on_beat(flow, now_us);
    }

    int on_beat(pitlane::FullFlow& flow, int64_t now_us) noexcept {
        pitlane::FlowCounters c{};
        flow.counters(c);
        if (now_us - last_host_us_ >= 1000000) {
            last_host_us_ = now_us;
            (void)pitlane::host_sample(host_);
        }

        const int64_t equity = kEquityBase + c.realized + c.unrealized;
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
            const pitlane::RiskAction act = guard_.evaluate(rin, ev);
            cp_.apply_risk(act, ev, now_us);
            if ((act == pitlane::RiskAction::BlockNew || act == pitlane::RiskAction::Halt) &&
                ev.code != last_risk_code_) {
                last_risk_code_ = ev.code;
                pitlane::OpsAuditRec rec{};
                rec.seq = ch_.alloc_seq();
                rec.code = ev.code;
                rec.kind = 0xff;
                rec.stamp_us = now_us;
                ch_.audit(rec);
            } else if (act == pitlane::RiskAction::Ok) {
                last_risk_code_ = 0;
            }
        }

        fill(snap, flow, c, now_us);
        board_.publish(snap);
        return 0;
    }

    void fill(pitlane::StatusSnapshot& snap, pitlane::FullFlow& flow, const pitlane::FlowCounters& c,
              int64_t now_us) noexcept {
        snap.seq = ++snap_seq_;
        snap.stamp_us = now_us;
        snap.last_tick_us = paper_ ? now_us : c.last_tick_us;
        snap.uptime_us = now_us > start_us_ ? now_us - start_us_ : 0;
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
        snap.equity = kEquityBase + c.realized + c.unrealized;
        snap.peak_equity = peak_equity_;
        snap.drawdown = peak_equity_ > snap.equity ? peak_equity_ - snap.equity : 0;
        snap.turnover = c.daily_notional;
        snap.fees = c.fees;
        snap.order_rate_1s = c.rate_1s;
        const int64_t all = c.submitted + c.rms_reject;
        snap.reject_rate_ppm = all > 0 ? (c.rejects + c.rms_reject) * 1000000 / all : 0;
        snap.cpu_ppm = host_.cpu_ppm;
        snap.mem_ppm = host_.mem_ppm;
        snap.load_1m_ppm = host_.load_1m_ppm;
        snap.threads = host_.threads;

        pitlane::FlowPos pos[pitlane::FullFlow::kMaxNames]{};
        const int np = flow.positions(pos, pitlane::FullFlow::kMaxNames);
        snap.pos_n = np > pitlane::kStatusPositions ? pitlane::kStatusPositions : np;
        for (int i = 0; i < snap.pos_n; ++i) {
            pitlane::StatusPosition& s = snap.pos[i];
            s.name = pos[i].name;
            s.today = pos[i].long_qty - pos[i].short_qty;
            s.yday = pos[i].long_yday - pos[i].short_yday;
            s.avg_px = pos[i].long_qty > 0 ? pos[i].avg_long_px : pos[i].avg_short_px;
            s.realized = pos[i].realized;
            s.unrealized = pos[i].unrealized;
            snap.net_qty += s.today;
        }
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

    void render_status(const pitlane::StatusSnapshot& s) noexcept {
        Out o{json_, kJsonCap, 0};
        o.add("{\"seq\":%llu,\"stamp_us\":%lld,\"state\":%u,\"md_up\":%u,\"td_up\":%u,\"strategy_on\":%u",
              static_cast<unsigned long long>(s.seq), static_cast<long long>(s.stamp_us),
              static_cast<unsigned>(s.state), static_cast<unsigned>(s.md_up), static_cast<unsigned>(s.td_up),
              static_cast<unsigned>(s.strategy_on));
        o.add(",\"halted\":%u,\"version\":%u,\"last_tick_us\":%lld,\"uptime_us\":%lld",
              static_cast<unsigned>(s.halted), static_cast<unsigned>(s.config_version),
              static_cast<long long>(s.last_tick_us), static_cast<long long>(s.uptime_us));
        o.add(",\"counters\":{\"quotes\":%lld,\"signals\":%lld,\"parents\":%lld,\"children\":%lld,\"fills\":%lld,"
              "\"rejects\":%lld,\"cancels\":%lld,\"orders_1s\":%lld,\"reject_ppm\":%lld}",
              static_cast<long long>(s.quotes), static_cast<long long>(s.signals), static_cast<long long>(s.parents),
              static_cast<long long>(s.child_orders), static_cast<long long>(s.fills),
              static_cast<long long>(s.rejects), static_cast<long long>(s.cancels),
              static_cast<long long>(s.order_rate_1s), static_cast<long long>(s.reject_rate_ppm));
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
        for (int i = 0; i < s.pos_n; ++i) {
            if (i) o.add(",");
            o.add("{\"name\":%u,\"today\":%lld,\"yday\":%lld,\"avg_px\":%lld,\"realized\":%lld,\"unrealized\":%lld}",
                  static_cast<unsigned>(s.pos[i].name), static_cast<long long>(s.pos[i].today),
                  static_cast<long long>(s.pos[i].yday), static_cast<long long>(s.pos[i].avg_px),
                  static_cast<long long>(s.pos[i].realized), static_cast<long long>(s.pos[i].unrealized));
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
        o.add("],\"note\":\"");
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
        o.add("{\"code\":%d,\"msg\":\"%s\",\"seq\":%llu,\"pending\":%u}", static_cast<int>(r.code), msg,
              static_cast<unsigned long long>(r.seq), pending ? 1u : 0u);
        json_len_ = o.n;
    }

    bool post(const pitlane::OpsCommand& cmd, pitlane::HttpResp& resp) noexcept {
        pitlane::OpsCommand c = cmd;
        c.seq = ch_.alloc_seq();
        c.id = c.id ? c.id : c.seq;
        if (!ch_.submit(c)) {
            static const char kBusy[] = "{\"code\":4,\"msg\":\"busy\"}";
            resp.json(503, kBusy, sizeof kBusy - 1);
            return true;
        }
        pitlane::OpsResult r{};
        for (int i = 0; i < 100; ++i) {
            if (ch_.result(c.seq, r)) {
                render_result(r, false);
                resp.json(200, json_, static_cast<uint32_t>(json_len_));
                return true;
            }
            timespec ts{0, 2000000};
            nanosleep(&ts, nullptr);
        }
        r.seq = c.seq;
        r.id = c.id;
        render_result(r, true);
        resp.json(202, json_, static_cast<uint32_t>(json_len_));
        return true;
    }

    bool handle(const pitlane::HttpReq& req, pitlane::HttpResp& resp) noexcept {
        if (std::strcmp(req.path, "/api/status") == 0) {
            pitlane::StatusSnapshot s{};
            if (!board_.read(s)) {
                static const char kBusy[] = "{\"code\":5,\"msg\":\"snapshot busy\"}";
                resp.json(503, kBusy, sizeof kBusy - 1);
                return true;
            }
            render_status(s);
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/events") == 0) {
            int64_t since = 0;
            (void)simnow_ops::key_i64(req.query, static_cast<uint32_t>(std::strlen(req.query)), "since", since);
            render_events(since);
            resp.json(200, json_, static_cast<uint32_t>(json_len_));
            return true;
        }
        if (std::strcmp(req.path, "/api/health") == 0) {
            static const char kOk[] = "{\"ok\":1}";
            resp.json(200, kOk, sizeof kOk - 1);
            return true;
        }
        if (std::strcmp(req.method, "POST") != 0) return false;
        pitlane::OpsCommand c{};
        if (std::strcmp(req.path, "/api/order") == 0) {
            int64_t name = 0;
            int64_t side = 0;
            int64_t qty = 0;
            if (!simnow_ops::key_i64(req.body, req.body_len, "name", name) ||
                !simnow_ops::key_i64(req.body, req.body_len, "side", side) ||
                !simnow_ops::key_i64(req.body, req.body_len, "qty", qty)) {
                return bad(resp, "name/side/qty");
            }
            c.kind = static_cast<uint8_t>(pitlane::OpsKind::Direct);
            c.name = static_cast<uint32_t>(name);
            c.side = static_cast<uint8_t>(side);
            c.qty = qty;
            (void)simnow_ops::key_i64(req.body, req.body_len, "px", c.px);
            return post(c, resp);
        }
        if (std::strcmp(req.path, "/api/basket") == 0) {
            const int n = simnow_ops::key_legs(req.body, req.body_len, c.basket, pitlane::kOpsBasketLegs);
            if (n <= 0) return bad(resp, "legs");
            c.kind = static_cast<uint8_t>(pitlane::OpsKind::Basket);
            c.legs = static_cast<uint16_t>(n);
            int64_t id = 0;
            if (simnow_ops::key_i64(req.body, req.body_len, "id", id) && id > 0) c.id = static_cast<uint64_t>(id);
            return post(c, resp);
        }
        if (std::strcmp(req.path, "/api/algo") == 0) {
            int64_t name = 0;
            int64_t side = 0;
            int64_t qty = 0;
            int64_t horizon_ms = 0;
            int64_t slice_ms = 0;
            if (!simnow_ops::key_i64(req.body, req.body_len, "name", name) ||
                !simnow_ops::key_i64(req.body, req.body_len, "side", side) ||
                !simnow_ops::key_i64(req.body, req.body_len, "qty", qty) ||
                !simnow_ops::key_i64(req.body, req.body_len, "horizon_ms", horizon_ms)) {
                return bad(resp, "name/side/qty/horizon_ms");
            }
            if (!simnow_ops::key_i64(req.body, req.body_len, "slice_ms", slice_ms)) slice_ms = horizon_ms / 10;
            if (slice_ms <= 0) slice_ms = 1;
            c.kind = static_cast<uint8_t>(pitlane::OpsKind::Algo);
            c.name = static_cast<uint32_t>(name);
            c.side = static_cast<uint8_t>(side);
            c.qty = qty;
            c.horizon_us = horizon_ms * 1000;
            c.slice_us = slice_ms * 1000;
            return post(c, resp);
        }
        if (std::strcmp(req.path, "/api/control") == 0) {
            char action[32];
            if (!simnow_ops::key_str(req.body, req.body_len, "action", action, sizeof action)) {
                return bad(resp, "action");
            }
            pitlane::OpsKind kind;
            if (std::strcmp(action, "pause") == 0) kind = pitlane::OpsKind::Pause;
            else if (std::strcmp(action, "resume") == 0) kind = pitlane::OpsKind::Resume;
            else if (std::strcmp(action, "halt") == 0) kind = pitlane::OpsKind::Halt;
            else if (std::strcmp(action, "resume_risk") == 0) kind = pitlane::OpsKind::ResumeRisk;
            else if (std::strcmp(action, "reload") == 0) kind = pitlane::OpsKind::Reload;
            else if (std::strcmp(action, "stop") == 0) kind = pitlane::OpsKind::Stop;
            else if (std::strcmp(action, "cancel_all") == 0) kind = pitlane::OpsKind::CancelAll;
            else {
                Out o{json_, kJsonCap, 0};
                o.add("{\"code\":%d,\"msg\":\"unknown action\"}", static_cast<int>(pitlane::kOpsBadReq));
                resp.json(400, json_, static_cast<uint32_t>(o.n));
                return true;
            }
            c.kind = static_cast<uint8_t>(kind);
            return post(c, resp);
        }
        return false;
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

    pitlane::OpsChannel<256> ch_{};
    pitlane::StatusBoard<4> board_{};
    pitlane::RiskGuard guard_{};
    pitlane::HttpServer http_{};
    pitlane::ControlPlane cp_{};
    pitlane::OpsPolicy pol_{};
    pitlane::HostSample host_{};
    pitlane::FullFlow* flow_{nullptr};
    bool paper_{true};
    int64_t start_us_{0};
    int64_t last_host_us_{0};
    int64_t last_risk_us_{0};
    int64_t last_snap_us_{0};
    int64_t peak_equity_{kEquityBase};
    uint64_t snap_seq_{0};
    uint8_t last_risk_code_{0};
    char root_[256]{};
    char conf_path_[512]{};
    char* json_{nullptr};
    int json_len_{0};
};

}  // namespace simnow_ops
