#include "pitlane/ops/command.hpp"
#include "pitlane/ops/control.hpp"
#include "pitlane/ops/host.hpp"
#include "pitlane/ops/http.hpp"
#include "pitlane/ops/status.hpp"
#include "pitlane/order/risk_guard.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

static int g_fails = 0;

static void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

static void check_status() {
    pitlane::StatusBoard<4> board;
    pitlane::StatusSnapshot in{};
    in.seq = 1;
    in.state = 1;
    in.fills = 42;
    std::snprintf(in.note, sizeof in.note, "hello");
    board.publish(in);
    pitlane::StatusSnapshot out{};
    expect(board.read(out), "status read");
    expect(out.seq == 1 && out.state == 1 && out.fills == 42 && std::strcmp(out.note, "hello") == 0, "status roundtrip");
    expect(board.version() == 2, "status version");

    in.seq = 2;
    in.fills = 43;
    board.publish(in);
    expect(board.read(out) && out.seq == 2 && out.fills == 43, "status second publish");
    expect(board.version() == 4, "status version monotonic");

    std::atomic<int> ok{0};
    std::atomic<bool> done{false};
    auto reader = [&]() {
        while (!done.load(std::memory_order_acquire)) {
            pitlane::StatusSnapshot s{};
            if (board.read(s) && s.seq > 0) ok.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread r1(reader);
    std::thread r2(reader);
    for (int i = 0; i < 4000; ++i) {
        pitlane::StatusSnapshot s{};
        s.seq = static_cast<uint64_t>(i + 10);
        s.fills = i;
        board.publish(s);
    }
    done.store(true, std::memory_order_release);
    r1.join();
    r2.join();
    expect(ok.load() > 0, "status concurrent readers");
}

static void check_channel() {
    pitlane::OpsChannel<8> ch;
    expect(ch.alloc_seq() == 1 && ch.alloc_seq() == 2, "channel seq");
    pitlane::OpsCommand c{};
    c.seq = 7;
    c.id = 9;
    c.kind = static_cast<uint8_t>(pitlane::OpsKind::Direct);
    expect(ch.submit(c), "channel submit");
    pitlane::OpsCommand got{};
    expect(ch.take(got) && got.seq == 7 && got.kind == c.kind, "channel take");

    pitlane::OpsResult r{};
    r.seq = 7;
    r.id = 9;
    r.code = pitlane::kOpsOk;
    ch.ack(r, 123);
    pitlane::OpsResult rout{};
    expect(ch.result(7, rout) && rout.code == pitlane::kOpsOk && rout.id == 9, "channel result");
    expect(!ch.result(8, rout), "channel missing result");

    pitlane::OpsAuditRec recs[4]{};
    expect(ch.recent_audit(recs, 4) == 1 && recs[0].seq == 7 && recs[0].kind == 0, "channel audit");

    // The send ring keeps one slot open; Cap - 1 entries fit.
    pitlane::OpsChannel<8> full;
    int accepted = 0;
    for (int i = 0; i < 16; ++i) {
        pitlane::OpsCommand x{};
        x.seq = static_cast<uint64_t>(i + 1);
        x.kind = 1;
        if (full.submit(x)) ++accepted;
    }
    expect(accepted == 7, "channel full rejects");
}

namespace {

int64_t g_net1 = 0;
int64_t g_net2 = 0;
int g_sent = 0;
pitlane::Intent g_last{};

int send_cb(void*, const pitlane::Intent& in) {
    ++g_sent;
    g_last = in;
    int64_t& net = in.name == 2 ? g_net2 : g_net1;
    net += in.side == 1 ? in.qty : -in.qty;
    return 0;
}

void top_cb(void*, uint32_t, pitlane::ManualQuote& q) {
    q.bid = 1000;
    q.ask = 1001;
}

int64_t net_cb(void*, uint32_t name) { return name == 2 ? g_net2 : g_net1; }

bool reload_cb(void* user, pitlane::OpsPolicy& out) {
    auto* pol = static_cast<pitlane::OpsPolicy*>(user);
    out = *pol;
    return true;
}

int run_control();

}  // namespace

static void check_control() {
    expect(run_control() == 0, "control plane");
}

namespace {

pitlane::OpsResult drain(pitlane::ControlPlane& cp, pitlane::OpsChannel<256>& ch, uint64_t seq, int64_t now) {
    pitlane::RiskInputs in{};
    in.now_us = now;
    in.last_tick_us = now;
    in.md_up = true;
    in.td_up = true;
    pitlane::StatusSnapshot snap{};
    cp.on_beat(now, in, snap);
    pitlane::OpsResult r{};
    (void)ch.result(seq, r);
    return r;
}

int run_control() {
    pitlane::OpsChannel<256> ch;
    pitlane::Rms rms;
    pitlane::OpsPolicy pol{};
    pol.max_manual_qty = 10;
    pol.max_manual_notional = 100000000;
    pol.max_manual_per_min = 100;
    pitlane::ControlPlane cp;
    cp.open(&ch, &rms, pol);
    pitlane::IdMint mint(1, 1);
    cp.set_mint(&mint);
    cp.set_account(1);
    cp.set_send(send_cb, nullptr);
    cp.set_top(top_cb, nullptr);
    cp.set_net(net_cb, nullptr);

    g_net1 = 0;
    g_net2 = 0;
    g_sent = 0;
    int64_t now = 1000000;

    pitlane::OpsCommand d{};
    d.seq = ch.alloc_seq();
    d.id = 1;
    d.kind = static_cast<uint8_t>(pitlane::OpsKind::Direct);
    d.side = 1;
    d.name = 1;
    d.qty = 2;
    expect(ch.submit(d), "control submit direct");
    pitlane::OpsResult r = drain(cp, ch, d.seq, now);
    expect(r.code == pitlane::kOpsOk && g_sent == 1, "control direct accepted");
    expect(g_last.name == 1 && g_last.qty == 2 && g_last.px == 1001 && g_last.style == 3, "control direct intent");
    expect(g_last.tag != 0, "control direct tag");

    d.seq = ch.alloc_seq();
    d.qty = 11;
    expect(ch.submit(d), "control submit over limit");
    r = drain(cp, ch, d.seq, now);
    expect(r.code == pitlane::kOpsRisk, "control qty limit");

    pitlane::OpsCommand pause{};
    pause.seq = ch.alloc_seq();
    pause.kind = static_cast<uint8_t>(pitlane::OpsKind::Pause);
    expect(ch.submit(pause), "control submit pause");
    r = drain(cp, ch, pause.seq, now);
    expect(r.code == pitlane::kOpsOk && cp.paused(), "control pause");

    d.seq = ch.alloc_seq();
    d.qty = 1;
    expect(ch.submit(d), "control submit paused direct");
    r = drain(cp, ch, d.seq, now);
    expect(r.code == pitlane::kOpsHalted, "control paused rejects");

    pitlane::OpsCommand resume{};
    resume.seq = ch.alloc_seq();
    resume.kind = static_cast<uint8_t>(pitlane::OpsKind::Resume);
    expect(ch.submit(resume), "control submit resume");
    r = drain(cp, ch, resume.seq, now);
    expect(r.code == pitlane::kOpsOk && !cp.paused(), "control resume");

    pitlane::OpsCommand halt{};
    halt.seq = ch.alloc_seq();
    halt.kind = static_cast<uint8_t>(pitlane::OpsKind::Halt);
    expect(ch.submit(halt), "control submit halt");
    r = drain(cp, ch, halt.seq, now);
    expect(r.code == pitlane::kOpsOk && cp.halted() && rms.halted(), "control halt");

    d.seq = ch.alloc_seq();
    d.qty = 1;
    expect(ch.submit(d), "control submit halted direct");
    r = drain(cp, ch, d.seq, now);
    expect(r.code == pitlane::kOpsHalted, "control halted rejects");

    pitlane::OpsCommand rr{};
    rr.seq = ch.alloc_seq();
    rr.kind = static_cast<uint8_t>(pitlane::OpsKind::ResumeRisk);
    expect(ch.submit(rr), "control submit resume risk");
    r = drain(cp, ch, rr.seq, now);
    expect(r.code == pitlane::kOpsOk && !cp.halted() && !rms.halted(), "control resume risk");

    pitlane::OpsPolicy next = pol;
    next.max_manual_qty = 5;
    cp.set_reload(reload_cb, &next);
    pitlane::OpsCommand reload{};
    reload.seq = ch.alloc_seq();
    reload.kind = static_cast<uint8_t>(pitlane::OpsKind::Reload);
    expect(ch.submit(reload), "control submit reload");
    const uint32_t before = cp.config_version();
    r = drain(cp, ch, reload.seq, now);
    expect(r.code == pitlane::kOpsOk && cp.config_version() == before + 1, "control reload version");

    d.seq = ch.alloc_seq();
    d.qty = 6;
    expect(ch.submit(d), "control submit reloaded limit");
    r = drain(cp, ch, d.seq, now);
    expect(r.code == pitlane::kOpsRisk, "control reloaded limit");

    // Basket converges to a target position and retires itself.
    g_net1 = 0;
    g_net2 = 0;
    g_sent = 0;
    cp.set_reload(nullptr, nullptr);
    pitlane::OpsCommand bk{};
    bk.seq = ch.alloc_seq();
    bk.id = 77;
    bk.kind = static_cast<uint8_t>(pitlane::OpsKind::Basket);
    bk.legs = 2;
    bk.basket[0].name = 1;
    bk.basket[0].target = 3;
    bk.basket[1].name = 2;
    bk.basket[1].target = -2;
    expect(ch.submit(bk), "control submit basket");
    r = drain(cp, ch, bk.seq, now);
    expect(r.code == pitlane::kOpsOk, "control basket accepted");
    expect(cp.manual().baskets_active() == 1, "control basket active");
    now += 300000;
    pitlane::RiskInputs in{};
    in.now_us = now;
    in.last_tick_us = now;
    pitlane::StatusSnapshot snap{};
    cp.on_beat(now, in, snap);
    expect(g_sent == 2 && g_net1 == 3 && g_net2 == -2, "control basket children");
    now += 300000;
    cp.on_beat(now, in, snap);
    expect(cp.manual().baskets_active() == 0, "control basket done");

    // TWAP slices the parent over the horizon.
    g_net1 = 0;
    g_net2 = 0;
    g_sent = 0;
    pitlane::OpsCommand al{};
    al.seq = ch.alloc_seq();
    al.id = 88;
    al.kind = static_cast<uint8_t>(pitlane::OpsKind::Algo);
    al.name = 1;
    al.side = 2;
    al.qty = 4;
    al.horizon_us = 4000000;
    al.slice_us = 1000000;
    expect(ch.submit(al), "control submit algo");
    r = drain(cp, ch, al.seq, now);
    expect(r.code == pitlane::kOpsOk && cp.manual().algos_active() == 1, "control algo accepted");
    int slices = 0;
    for (int i = 0; i < 4; ++i) {
        now += 1000000;
        cp.on_beat(now, in, snap);
        if (g_last.side == 2) ++slices;
    }
    expect(g_sent == 4 && cp.manual().algos_active() == 0, "control algo slices");
    (void)slices;

    pitlane::OpsCommand stop{};
    stop.seq = ch.alloc_seq();
    stop.kind = static_cast<uint8_t>(pitlane::OpsKind::Stop);
    expect(ch.submit(stop), "control submit stop");
    r = drain(cp, ch, stop.seq, now);
    expect(r.code == pitlane::kOpsOk && cp.stopping() && cp.state() == 4, "control stop");
    return 0;
}

}  // namespace

static void check_risk_guard() {
    pitlane::OpsPolicy pol{};
    pitlane::RiskGuard g;
    pol.max_daily_loss = 1000;
    pol.max_drawdown_ppm = 100000;
    pol.max_reject_ppm = 300000;
    pol.stale_tick_us = 5000000;
    g.set(pol);

    pitlane::RiskInputs in{};
    in.now_us = 10000000;
    in.last_tick_us = in.now_us;
    in.peak_equity = 1000;
    in.equity = 1000;
    pitlane::RiskEvent ev{};
    expect(g.evaluate(in, ev) == pitlane::RiskAction::Ok, "risk ok");

    in.daily_pnl = -1000;
    expect(g.evaluate(in, ev) == pitlane::RiskAction::Halt && ev.code == pitlane::kRiskDailyLoss, "risk daily loss");
    in.daily_pnl = 0;
    expect(g.evaluate(in, ev) == pitlane::RiskAction::Halt, "risk halt sticky");
    g.resume();
    expect(g.evaluate(in, ev) == pitlane::RiskAction::Ok, "risk resume");

    in.equity = 800;
    expect(g.evaluate(in, ev) == pitlane::RiskAction::Halt && ev.code == pitlane::kRiskDrawdown, "risk drawdown");
    g.resume();
    in.equity = 1000;

    in.orders = 100;
    in.rejects = 40;
    expect(g.evaluate(in, ev) == pitlane::RiskAction::BlockNew && ev.code == pitlane::kRiskRejectRate, "risk rejects");
    in.rejects = 0;
    expect(g.evaluate(in, ev) == pitlane::RiskAction::Ok, "risk rejects cleared");

    in.md_up = false;
    expect(g.evaluate(in, ev) == pitlane::RiskAction::BlockNew && ev.code == pitlane::kRiskMdDown, "risk md down");
    in.md_up = true;
    in.td_up = false;
    expect(g.evaluate(in, ev) == pitlane::RiskAction::BlockNew && ev.code == pitlane::kRiskTdDown, "risk td down");
    in.td_up = true;

    in.last_tick_us = in.now_us - 6000000;
    expect(g.evaluate(in, ev) == pitlane::RiskAction::BlockNew && ev.code == pitlane::kRiskStaleTick, "risk stale");
}

namespace {

bool http_handler(void*, const pitlane::HttpReq& req, pitlane::HttpResp& resp) {
    if (std::strcmp(req.path, "/api/status") == 0) {
        static const char kJson[] = "{\"ok\":1}";
        resp.json(200, kJson, sizeof kJson - 1);
        return true;
    }
    if (std::strcmp(req.path, "/api/echo") == 0 && req.body_len == 4) {
        resp.json(200, req.body, req.body_len);
        return true;
    }
    return false;
}

int http_get(uint16_t port, const char* req, char* out, int cap) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    const std::size_t n = std::strlen(req);
    if (::send(fd, req, n, 0) != static_cast<ssize_t>(n)) {
        ::close(fd);
        return -1;
    }
    int total = 0;
    for (;;) {
        const ssize_t got = ::recv(fd, out + total, static_cast<std::size_t>(cap - 1 - total), 0);
        if (got <= 0) break;
        total += static_cast<int>(got);
        if (total >= cap - 1) break;
    }
    out[total] = 0;
    ::close(fd);
    return total;
}

}  // namespace

static void check_http() {
    pitlane::HttpServer srv;
    expect(srv.start(0, http_handler, nullptr, nullptr), "http start");
    expect(srv.port() != 0, "http ephemeral port");
    char buf[2048];
    const int n = http_get(srv.port(), "GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n", buf, sizeof buf);
    expect(n > 0 && std::strstr(buf, "200 OK") && std::strstr(buf, "{\"ok\":1}"), "http status");
    const int m = http_get(srv.port(),
                           "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nabcd", buf, sizeof buf);
    expect(m > 0 && std::strstr(buf, "200 OK") && std::strstr(buf, "abcd"), "http post body");
    const int b = http_get(srv.port(), "GET / HTTP/1.1\r\nHost: x\r\n\r\n", buf, sizeof buf);
    expect(b > 0 && std::strstr(buf, "503"), "http missing document");
    const int bad = http_get(srv.port(), "BROKEN\r\n\r\n", buf, sizeof buf);
    expect(bad > 0 && std::strstr(buf, "400"), "http bad request");
    srv.stop();
    expect(!srv.running(), "http stopped");
}

static void check_host() {
    pitlane::HostSample s{};
    expect(pitlane::host_sample(s), "host sample");
    expect(s.cpu_ppm >= 0 && s.cpu_ppm <= 1000000, "host cpu range");
    expect(s.mem_ppm >= 0 && s.mem_ppm <= 1000000, "host mem range");
    expect(s.load_1m_ppm >= 0 && s.threads >= 1, "host load and threads");
}

int main() {
    check_status();
    check_channel();
    check_control();
    check_risk_guard();
    check_http();
    check_host();
    if (g_fails == 0) std::printf("ops: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
