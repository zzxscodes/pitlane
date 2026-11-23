#include "pitlane/clear/clearing.hpp"
#include "pitlane/clear/report.hpp"
#include "pitlane/clear/statement.hpp"
#include "pitlane/ops/accounts.hpp"
#include "pitlane/ops/alarm.hpp"
#include "pitlane/ops/canary.hpp"
#include "pitlane/ops/logsearch.hpp"
#include "pitlane/ops/replayctl.hpp"
#include "pitlane/ops/seats.hpp"
#include "pitlane/ops/command.hpp"
#include "pitlane/ops/control.hpp"
#include "pitlane/ops/auth.hpp"
#include "pitlane/ops/gateway.hpp"
#include "pitlane/ops/group_risk.hpp"
#include "pitlane/ops/health.hpp"
#include "pitlane/ops/host.hpp"
#include "pitlane/ops/http.hpp"
#include "pitlane/ops/instruments.hpp"
#include "pitlane/ops/series.hpp"
#include "pitlane/ops/sha256.hpp"
#include "pitlane/ops/status.hpp"
#include "pitlane/ops/strategy.hpp"
#include "pitlane/order/risk_guard.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
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

// Tests can run concurrently (ctest -j), so every scratch file carries the pid.
static void ci_path(char* out, int cap, const char* base) {
    std::snprintf(out, static_cast<std::size_t>(cap), "%s.%ld", base, static_cast<long>(::getpid()));
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
    in.ack_timeouts = 3;
    in.life_timeouts = 4;
    in.pull_timeouts = 5;
    board.publish(in);
    expect(board.read(out) && out.seq == 2 && out.fills == 43, "status second publish");
    expect(out.ack_timeouts == 3 && out.life_timeouts == 4 && out.pull_timeouts == 5, "status timeouts");
    expect(sizeof(pitlane::StatusSnapshot) < 1024, "status stays compact");
    expect(board.version() == 4, "status version monotonic");

    std::atomic<int> ok{0};
    std::atomic<int> seen{0};
    std::atomic<bool> done{false};
    auto reader = [&]() {
        bool got = false;
        while (!done.load(std::memory_order_acquire)) {
            pitlane::StatusSnapshot s{};
            if (board.read(s) && s.seq > 0) {
                ok.fetch_add(1, std::memory_order_relaxed);
                got = true;
            }
        }
        if (got) seen.fetch_add(1, std::memory_order_release);
    };
    std::thread r1(reader);
    std::thread r2(reader);
    for (int i = 0; i < 4000; ++i) {
        pitlane::StatusSnapshot s{};
        s.seq = static_cast<uint64_t>(i + 10);
        s.fills = i;
        board.publish(s);
    }
    // The board keeps a valid snapshot published, so a reader that gets
    // scheduled late still observes one. Wait (bounded) for both of them
    // before tearing the threads down.
    for (int spin = 0; spin < 4000000 && seen.load(std::memory_order_acquire) < 2; ++spin) {
        std::this_thread::yield();
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
int g_send_rc = 0;
pitlane::Intent g_last{};

int send_cb(void*, const pitlane::Intent& in) {
    ++g_sent;
    g_last = in;
    if (g_send_rc != 0) return g_send_rc;
    int64_t& net = in.name == 2 ? g_net2 : g_net1;
    net += in.side == 1 ? in.qty : -in.qty;
    return 0;
}

// Stands in for the strategy side of the short restriction.
const char* short_reason_cb(void*) { return "short not allowed on 1 (0 available)"; }

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

    // A strategy-side rejection reason must reach the ticket ack.
    cp.set_reason(short_reason_cb, nullptr);
    g_send_rc = 1;
    d.seq = ch.alloc_seq();
    d.qty = 1;
    expect(ch.submit(d), "control submit rejected direct");
    r = drain(cp, ch, d.seq, now);
    expect(r.code == pitlane::kOpsRisk && std::strstr(r.msg, "short not allowed") != nullptr,
           "control reject reason");
    g_send_rc = 0;

    d.seq = ch.alloc_seq();
    d.qty = 1;
    d.acct = 2;
    expect(ch.submit(d), "control submit acct");
    r = drain(cp, ch, d.seq, now);
    expect(r.code == pitlane::kOpsOk && g_last.acct == 2, "control account override");
    d.acct = 0;

    // A direct close carries its offset and an unknown offset is refused.
    d.seq = ch.alloc_seq();
    d.off = static_cast<uint8_t>(pitlane::Off::CloseToday);
    expect(ch.submit(d), "control submit close today");
    r = drain(cp, ch, d.seq, now);
    expect(r.code == pitlane::kOpsOk && g_last.off == pitlane::Off::CloseToday, "control offset forwarded");
    d.seq = ch.alloc_seq();
    d.off = 9;
    expect(ch.submit(d), "control submit bad offset");
    r = drain(cp, ch, d.seq, now);
    expect(r.code == pitlane::kOpsBadReq, "control bad offset refused");
    d.off = 0;

    d.seq = ch.alloc_seq();
    d.qty = 11;
    expect(ch.submit(d), "control submit over limit");
    r = drain(cp, ch, d.seq, now);
    expect(r.code == pitlane::kOpsRisk, "control qty limit");
    expect(r.msg[0] != 0 && std::strstr(r.msg, "qty") != nullptr, "control reject reason");

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

    pitlane::OpsCommand rl{};
    rl.seq = ch.alloc_seq();
    rl.kind = static_cast<uint8_t>(pitlane::OpsKind::RiskLimits);
    rl.limits.max_order_qty = 3;
    expect(ch.submit(rl), "control submit risk limits");
    r = drain(cp, ch, rl.seq, now);
    expect(r.code == pitlane::kOpsOk && rms.limits().max_order_qty == 3, "control risk limits applied");
    rl.seq = ch.alloc_seq();
    rl.limits.max_order_qty = 0;
    expect(ch.submit(rl), "control submit risk limits reset");
    r = drain(cp, ch, rl.seq, now);
    expect(r.code == pitlane::kOpsOk && rms.limits().max_order_qty == 0, "control risk limits reset");

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
    if (std::strcmp(req.path, "/api/echo") == 0 && req.body_len == 4 &&
        std::strncmp(req.body, "abcd", 4) == 0 && req.body[4] == '\0') {
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

// Sends the head, waits so the body lands in a second segment, then sends it.
int http_post_split(uint16_t port, const char* head, const char* body, char* out, int cap) {
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
    const std::size_t nh = std::strlen(head);
    const std::size_t nb = std::strlen(body);
    if (::send(fd, head, nh, 0) != static_cast<ssize_t>(nh)) {
        ::close(fd);
        return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (::send(fd, body, nb, 0) != static_cast<ssize_t>(nb)) {
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
    const int e = http_get(srv.port(),
                           "POST /api/status HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", buf, sizeof buf);
    expect(e > 0 && std::strstr(buf, "200 OK") && std::strstr(buf, "{\"ok\":1}"), "http empty post");
    const int s = http_post_split(srv.port(),
                                  "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\n", "abcd",
                                  buf, sizeof buf);
    expect(s > 0 && std::strstr(buf, "200 OK") && std::strstr(buf, "abcd"), "http split body terminated");
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

static void check_sha() {
    char hex[65];
    uint8_t d[32];
    pitlane::sha256("abc", 3, d);
    pitlane::hex_encode(d, 32, hex);
    expect(std::strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256 abc");

    pitlane::hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    pitlane::hex_encode(d, 32, hex);
    expect(std::strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0, "hmac rfc4231");

    pitlane::pbkdf2_sha256("password", 8, reinterpret_cast<const uint8_t*>("salt"), 4, 1, d);
    pitlane::hex_encode(d, 32, hex);
    expect(std::strcmp(hex, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b") == 0, "pbkdf2 one round");

    pitlane::pbkdf2_sha256("password", 8, reinterpret_cast<const uint8_t*>("salt"), 4, 4096, d);
    pitlane::hex_encode(d, 32, hex);
    expect(std::strcmp(hex, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a") == 0, "pbkdf2 4096 rounds");
}

static void check_auth() {
    pitlane::OpsUserStore users;
    expect(users.add("alice", "secret1", pitlane::kRoleTrader, false, nullptr), "auth add user");
    expect(!users.add("alice", "secret1", pitlane::kRoleTrader, false, nullptr), "auth duplicate rejected");
    expect(!users.add("bob", "short", pitlane::kRoleViewer, false, nullptr), "auth weak password rejected");
    expect(!users.add("bad name", "secret1", pitlane::kRoleViewer, false, nullptr), "auth bad name rejected");

    uint8_t role = 0;
    uint32_t perms = 0;
    expect(users.verify("alice", "secret1", role, perms), "auth verify");
    expect(role == pitlane::kRoleTrader && pitlane::ops_has(perms, pitlane::kPermTrade), "auth trader perms");
    expect(!users.verify("alice", "secret2", role, perms), "auth wrong password");
    expect(!users.verify("nobody", "secret1", role, perms), "auth unknown user");

    expect(users.set_role("alice", pitlane::kRoleRisk, false, nullptr), "auth set role");
    expect(users.verify("alice", "secret1", role, perms) && role == pitlane::kRoleRisk, "auth role applied");
    expect(pitlane::ops_has(perms, pitlane::kPermRiskControl), "auth risk control perm");
    expect(!pitlane::ops_has(perms, pitlane::kPermTrade), "auth risk cannot trade");

    char path[96];
    ci_path(path, sizeof path, "ops_auth_ci.conf");
    expect(users.save(path), "auth save");
    pitlane::OpsUserStore reloaded;
    expect(reloaded.load(path) && reloaded.count() == 1, "auth load");
    expect(reloaded.verify("alice", "secret1", role, perms) && role == pitlane::kRoleRisk, "auth reloaded verify");
    std::remove(path);

    pitlane::OpsSessionStore sessions;
    const int idx = sessions.create(reloaded.at(0), 1000, nullptr);
    expect(idx >= 0, "auth session create");
    expect(sessions.find(reloaded.at(0).salt, 1000) < 0, "auth session rejects junk");
    const char* tok = sessions.at(idx).token;
    expect(sessions.find(tok, 1000) == idx, "auth session find");
    expect(sessions.find(tok, 1000 + pitlane::OpsSessionStore::kTtlUs + 1) < 0, "auth session expiry");
    sessions.drop(tok);
    expect(sessions.find(tok, 1000) < 0, "auth session drop");

    char out[65];
    expect(pitlane::ops_token_from("Bearer 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", nullptr,
                                   out, sizeof out),
           "auth token header");
    expect(std::strcmp(out, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0, "auth token value");
    expect(pitlane::ops_token_from(nullptr,
                                   "a=1; pitlane_token=abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789; b=2",
                                   out, sizeof out),
           "auth token cookie");
    expect(!pitlane::ops_token_from("Bearer short", nullptr, out, sizeof out), "auth token rejects short");
}

static void check_gateway() {
    char path[96];
    ci_path(path, sizeof path, "ops_gateway_ci.conf");
    std::FILE* f = std::fopen(path, "w");
    expect(f != nullptr, "gateway conf open");
    if (f) {
        std::fprintf(f, "# comment\n");
        std::fprintf(f, "gateway = md-tape md tape 1 1\n");
        std::fprintf(f, "gateway = td-paper td paper 1 0\n");
        std::fclose(f);
    }
    pitlane::GatewayRegistry reg;
    expect(reg.load(path), "gateway load");
    expect(reg.count() == 2, "gateway count");
    expect(std::strcmp(reg.spec(0).id, "md-tape") == 0 && reg.spec(0).kind == pitlane::kGwMd, "gateway md spec");
    expect(reg.spec(0).auto_start, "gateway auto start");
    expect(reg.spec(1).kind == pitlane::kGwTd, "gateway td spec");
    expect(reg.index_of("td-paper") == 1 && reg.index_of("nope") < 0, "gateway index");
    reg.mark(0, pitlane::kGwRunning, "ok", 5);
    reg.mark(1, pitlane::kGwFailed, "boom", 6);
    expect(reg.running_count() == 1 && reg.failed_count() == 1, "gateway counters");
    expect(std::strcmp(pitlane::gw_state_name(reg.status(1).state), "failed") == 0, "gateway state name");
    reg.set_available(1, true);
    expect(reg.spec(1).available, "gateway availability");
    std::remove(path);
}

static void check_series() {
    pitlane::SeriesRing<8> ring;
    for (int i = 0; i < 5; ++i) {
        pitlane::SeriesTick t{};
        t.ts_us = static_cast<int64_t>(i) * 1000000ll;
        t.last = 100 + i;
        ring.push(t);
    }
    expect(ring.size() == 5 && ring.at(0).last == 100 && ring.at(4).last == 104, "series order");
    pitlane::SeriesBar bars[8]{};
    expect(ring.bars(5000000, 8, bars) == 1 && bars[0].open == 100 && bars[0].close == 104, "series one bar");
    expect(ring.bars(1000000, 8, bars) == 5 && bars[4].high == 104, "series per second");

    for (int i = 0; i < 10; ++i) ring.push(pitlane::SeriesTick{static_cast<int64_t>(i) * 1000000ll, 0, 0, i, 0, 0});
    expect(ring.size() == 8 && ring.at(0).last == 2, "series overwrite");

    char path[96];
    ci_path(path, sizeof path, "ops_series_ci.csv");
    std::FILE* f = std::fopen(path, "w");
    expect(f != nullptr, "series csv open");
    if (f) {
        std::fprintf(f, "1000000,7,100,101,100,1,2\n");
        std::fprintf(f, "2000000,7,101,102,101,1,2\n");
        std::fprintf(f, "3000000,8,200,201,200,1,2\n");
        std::fprintf(f, "4000000,7,0,0,0,0,0\n");
        std::fclose(f);
    }
    pitlane::SeriesTick out[8]{};
    expect(pitlane::series_read_tape(path, 7, 0, 9000000, 8, out) == 2, "series tape read");
    expect(out[1].last == 101, "series tape value");
    pitlane::SeriesSet set;
    expect(pitlane::series_seed_tape(set, path) == 3 && set.count() == 2, "series seed");
    expect(set.find(7) != nullptr && set.find(7)->size() == 2, "series seed ring");
    std::remove(path);

    // 夜盘跨 00:00：回退的日内时间应接成连续会话时钟，而不是被丢弃。
    constexpr int64_t kDayUs = 86400ll * 1000000;
    std::FILE* nf = std::fopen(path, "w");
    expect(nf != nullptr, "series night csv open");
    if (nf) {
        std::fprintf(nf, "75600000000,7,100,101,100,1,2\n");
        std::fprintf(nf, "86399000000,7,101,102,101,1,2\n");
        std::fprintf(nf, "1000000,7,102,103,102,1,2\n");
        std::fprintf(nf, "9000000000,7,103,104,103,1,2\n");
        std::fclose(nf);
    }
    expect(pitlane::series_read_tape(path, 7, 0, INT64_MAX, 8, out) == 4, "series night tape read");
    expect(out[2].ts_us == kDayUs + 1000000ll && out[3].ts_us == kDayUs + 9000000000ll, "series night rollover");
    pitlane::SeriesSet nset;
    expect(pitlane::series_seed_tape(nset, path) == 4 && nset.find(7) != nullptr &&
               nset.find(7)->size() == 4,
           "series night seed");
    expect(nset.find(7) != nullptr && nset.find(7)->at(3).ts_us == kDayUs + 9000000000ll,
           "series night seed clock");
    std::remove(path);
}

static void check_accounts() {
    char path[96];
    ci_path(path, sizeof path, "ops_accounts_ci.conf");
    std::FILE* f = std::fopen(path, "w");
    expect(f != nullptr, "accounts open");
    if (f) {
        std::fprintf(f, "# accounts\n");
        std::fprintf(f, "account = sim-test PitPaper astock CNY 1000 test\n");
        std::fprintf(f, "account = sim-live PitLive future CNY 2000 live\n");
        std::fprintf(f, "account = broken only-four-fields\n");
        std::fprintf(f, "account = sim-test Duplicate astock CNY 1 test\n");
        std::fclose(f);
    }
    pitlane::AccountRegistry reg;
    expect(reg.load(path), "accounts load");
    expect(reg.count() == 2, "accounts count");
    expect(std::strcmp(reg.spec(0).id, "sim-test") == 0 && reg.spec(0).num == 1, "accounts first num");
    expect(reg.spec(0).env == pitlane::kEnvTest && reg.spec(1).env == pitlane::kEnvLive, "accounts env");
    expect(reg.spec(1).num == 2 && reg.spec(1).cash == 2000, "accounts second");
    expect(reg.resolve("2") == 2 && reg.resolve("sim-test") == 1, "accounts resolve");
    expect(reg.resolve("nope") == 0, "accounts resolve unknown");
    expect(reg.index_of_num(2) == 1 && reg.index_of_num(9) < 0, "accounts by num");
    expect(reg.count_env(pitlane::kEnvTest) == 1 && reg.count_env(pitlane::kEnvLive) == 1, "accounts env count");
    std::remove(path);
}

static void check_instruments() {
    char path[96];
    ci_path(path, sizeof path, "ops_instruments_ci.conf");
    std::FILE* f = std::fopen(path, "w");
    expect(f != nullptr, "instruments open");
    if (f) {
        std::fprintf(f, "instrument = 1 astock CNY 100 1 0 250 min_fee=500 stamp_ppm=500\n");
        std::fprintf(f, "instrument = 3 future CNY 200 10 100000 23 short=1 gw=sim-td\n");
        std::fprintf(f, "instrument = 4 option CNY 10 100 120000 30\n");
        std::fprintf(f, "instrument = 5 future CNY 1 10 0 0 lot=100 min_qty=200 max_qty=5000"
                        " tickb=0:100:1,100:500:5,500:0:10\n");
        std::fprintf(f, "instrument = 6 astock CNY 1 1 0 0 lot=0\n");
        std::fprintf(f, "instrument = 7 astock CNY 1 1 0 0 lot=100 min_qty=500 max_qty=100\n");
        std::fprintf(f, "instrument = 8 astock CNY 1 1 0 0 tickb=bad,0:100:0\n");
        std::fprintf(f, "instrument = 9 nonsense CNY 1 1 0 0\n");
        std::fprintf(f, "instrument = 1 astock CNY 1 1 0 0\n");
        std::fclose(f);
    }
    pitlane::InstrumentRegistry reg;
    expect(reg.load(path), "instruments load");
    expect(reg.count() == 5, "instruments count");
    const pitlane::InstrSpec* a = reg.by_name(1);
    expect(a && a->klass == pitlane::kInstrAStock && a->min_fee == 500 && a->stamp_ppm == 500, "instrument astock");
    expect(a && !a->allow_short, "instrument astock no short");
    expect(a && a->lot == 1 && a->min_qty == 1 && a->max_qty == 0, "instrument default order size");
    const pitlane::InstrSpec* fut = reg.by_name(3);
    expect(fut && fut->klass == pitlane::kInstrFuture && fut->mult == 10 && fut->allow_short, "instrument future");
    expect(fut && std::strcmp(fut->gw, "sim-td") == 0, "instrument gateway");
    expect(reg.by_name(4) && reg.by_name(4)->allow_short, "instrument option short");
    const pitlane::InstrSpec* sized = reg.by_name(5);
    expect(sized && sized->lot == 100 && sized->min_qty == 200 && sized->max_qty == 5000, "instrument order size");
    expect(sized && sized->px_step(50) == 1 && sized->px_step(150) == 5 && sized->px_step(900) == 10,
           "instrument tick bands");
    expect(reg.px_step(5, 150) == 5 && reg.px_step(99, 150) == 0, "instrument registry tick");
    expect(sized && sized->qty_ok(200) && sized->qty_ok(5000) && !sized->qty_ok(100) && !sized->qty_ok(250) &&
               !sized->qty_ok(5100),
           "instrument order quantity");
    expect(reg.qty_ok(5, 300) && !reg.qty_ok(5, 150) && !reg.qty_ok(99, 100), "instrument registry quantity");
    expect(reg.by_name(6) == nullptr && reg.by_name(7) == nullptr, "instrument bad size skipped");
    expect(reg.by_name(8) && reg.by_name(8)->band_n == 0 && reg.by_name(8)->px_step(900) == 1,
           "instrument bad bands fall back");
    expect(reg.by_name(9) == nullptr, "instrument invalid skipped");
    std::remove(path);
}

static void check_instrument_capacity() {
    char path[96];
    ci_path(path, sizeof path, "ops_instruments_cap_ci.conf");
    std::FILE* f = std::fopen(path, "w");
    expect(f != nullptr, "instrument capacity open");
    if (f) {
        for (int i = 1; i <= 64; ++i) std::fprintf(f, "instrument = %d astock CNY 1 1 0 0\n", i);
        std::fclose(f);
    }
    pitlane::InstrumentRegistry reg;
    expect(reg.load(path), "instrument capacity load");
    expect(reg.count() == 64, "instrument capacity count");
    expect(reg.by_name(64) != nullptr && reg.by_name(65) == nullptr, "instrument capacity lookup");
    std::remove(path);
}

static void check_gateway_env() {
    char path[96];
    ci_path(path, sizeof path, "ops_gateway_env_ci.conf");
    std::FILE* f = std::fopen(path, "w");
    expect(f != nullptr, "gateway env open");
    if (f) {
        std::fprintf(f, "gateway = sim-md md tape 1 1 test\n");
        std::fprintf(f, "gateway = ctp-td td ctp 2 0 live\n");
        std::fprintf(f, "gateway = legacy-td td paper 1 0\n");
        std::fclose(f);
    }
    pitlane::GatewayRegistry reg;
    expect(reg.load(path), "gateway env load");
    expect(reg.count() == 3, "gateway env count");
    expect(reg.spec(0).env_bit == pitlane::kEnvTest && reg.spec(1).env_bit == pitlane::kEnvLive, "gateway env bits");
    expect(std::strcmp(reg.spec(1).env, "live") == 0, "gateway live env");
    expect(reg.spec(2).env_bit == pitlane::kEnvTest, "gateway legacy defaults to test");
    expect(reg.count_env(pitlane::kEnvTest) == 2 && reg.count_env(pitlane::kEnvLive) == 1, "gateway env count");
    reg.set_account_num(1, 2);
    expect(reg.spec(1).acct_num == 2, "gateway account num");
    std::remove(path);
}

static void check_alarms() {
    pitlane::AlarmBoard board;
    expect(board.claimed() == 0, "alarm empty");
    const int64_t s1 = board.push(pitlane::kAlarmReject, pitlane::kAlarmWarn, pitlane::kEnvTest, 10, "bad %d", 1);
    const int64_t s2 = board.push(pitlane::kAlarmJump, pitlane::kAlarmCritical, pitlane::kEnvLive, 20, "jump");
    const int64_t s3 = board.push(pitlane::kAlarmGateway, pitlane::kAlarmInfo, pitlane::kEnvTest, 30, "gw");
    expect(s1 == 1 && s2 == 2 && s3 == 3, "alarm seq");
    pitlane::Alarm out[8]{};
    expect(board.recent(out, 8, 0, pitlane::kEnvTest, true) == 2, "alarm env filter");
    expect(out[0].seq == 1 && out[1].seq == 3, "alarm filtered seq");
    expect(std::strcmp(out[0].text, "bad 1") == 0 && out[1].sev == pitlane::kAlarmInfo, "alarm text/sev");
    expect(board.recent(out, 8, 1, pitlane::kEnvLive, true) == 1 && out[0].seq == 2, "alarm since");
    expect(board.recent(out, 1, 0, pitlane::kEnvTest, false) == 1 && out[0].seq == 3, "alarm cap keeps newest");
    expect(board.claimed() == 3, "alarm claimed");
}

static void check_clearing() {
    pitlane::ClearingBook b;
    b.reset(1, pitlane::kEnvTest);
    pitlane::ClrFillIn f{};
    f.acct = 1;
    f.name = 1;
    f.side = 1;
    f.qty = 10;
    f.px = 1000000;
    pitlane::ClrFillOut o = b.apply(f);
    expect(o.opened_qty == 10 && o.realized == 0, "clearing open long");

    f.side = 2;
    f.qty = 4;
    f.px = 1100000;
    o = b.apply(f);
    expect(o.realized == 40 && o.closed_qty == 4, "clearing fifo close");
    expect(b.net_qty(1) == 6, "clearing net qty");

    uint32_t names[1] = {1};
    int64_t marks[1] = {900000};
    int64_t unreal = 0;
    b.unrealized(names, marks, 1, unreal);
    expect(unreal == -60, "clearing unrealized long");

    pitlane::ClearingBook s;
    s.reset(2, pitlane::kEnvTest);
    pitlane::ClrFillIn g{};
    g.acct = 2;
    g.name = 3;
    g.side = 2;
    g.qty = 5;
    g.px = 2000000;
    g.mult = 10;
    g.margin_ppm = 100000;
    g.allow_short = true;
    o = s.apply(g);
    expect(o.opened_qty == 5 && s.net_qty(3) == -5, "clearing open short");
    g.side = 1;
    g.qty = 2;
    g.px = 1900000;
    o = s.apply(g);
    expect(o.realized == 200, "clearing short pnl");
    int64_t marks2[1] = {2000000};
    uint32_t names2[1] = {3};
    expect(s.margin(names2, marks2, 1) == 600, "clearing margin");

    pitlane::ClearingBook fb;
    fb.reset(1, pitlane::kEnvTest);
    pitlane::ClrFillIn h{};
    h.acct = 1;
    h.name = 1;
    h.side = 2;
    h.qty = 1000;
    h.px = 1000000;
    h.fee_ppm = 250;
    h.stamp_ppm = 500;
    h.allow_short = true;
    o = fb.apply(h);
    expect(o.fee == 75, "clearing fee plus stamp");

    pitlane::ClrFillIn m{};
    m.acct = 1;
    m.name = 1;
    m.side = 1;
    m.qty = 1;
    m.px = 100;
    m.fee_ppm = 250;
    m.min_fee = 500;
    o = fb.apply(m);
    expect(o.fee == 500, "clearing minimum fee");

    // Commission and stamp duty are tracked apart, with equity and availability.
    pitlane::ClearingBook cb;
    cb.reset(1, pitlane::kEnvTest);
    cb.set_cash(1000000);
    pitlane::ClrFillIn c1{};
    c1.acct = 1;
    c1.name = 2;
    c1.side = 1;
    c1.qty = 10;
    c1.px = 1000000;
    c1.fee_ppm = 250;
    c1.stamp_ppm = 1000;
    c1.min_fee = 500;
    pitlane::ClrFillOut co = cb.apply(c1, 111);
    expect(co.commission == 500 && co.stamp == 0 && co.fee == 500, "clearing commission split");
    pitlane::ClrTrade blot[8]{};
    expect(cb.trades_since(0, blot, 8) == 1 && blot[0].name == 2 && blot[0].qty == 10 && blot[0].stamp_us == 111,
           "clearing blotter");
    expect(cb.trades_since(1, blot, 8) == 0, "clearing blotter since");
    pitlane::ClrFillIn c2 = c1;
    c2.side = 2;
    co = cb.apply(c2, 222);
    expect(co.commission == 500 && co.stamp == 1 && co.fee == 501, "clearing stamp on sell");
    expect(co.realized == 0, "clearing flat round trip");
    uint32_t cnames[1] = {2};
    int64_t cmarks[1] = {1000000};
    pitlane::ClearingReport crep{};
    cb.report(cnames, cmarks, 1, pitlane::kEnvTest, crep);
    expect(crep.trades == 2 && crep.turnover == 2000 && crep.commission == 1000 && crep.stamp == 1,
           "clearing totals");
    expect(crep.cash_in == 1000000 && crep.cash_flow == -1001 && crep.equity == 998999 && crep.available == 998999,
           "clearing equity");
    expect(crep.day_trades == 2 && crep.day_fees == 1001, "clearing day totals");
    cb.settle(333);
    cb.report(cnames, cmarks, 1, pitlane::kEnvTest, crep);
    expect(crep.day_trades == 0 && crep.day_fees == 0 && crep.settle_us == 333 && crep.day_seq == 1,
           "clearing settle resets day");
    expect(crep.trades == 2 && crep.fees == 1001 && crep.realized == 0, "clearing carries after settle");

    pitlane::Pms<64> pms;
    pitlane::PmsFill pf{};
    pf.acct = 1;
    pf.name = 1;
    pf.side = 1;
    pf.qty = 10;
    pf.px = 1000000;
    expect(pms.apply(pf), "clearing pms open");
    pitlane::ClearingReport rep{};
    b.report(names, marks, 1, pitlane::kEnvTest, rep);
    expect(rep.rows == 1 && rep.realized == 40 && rep.unrealized == -60, "clearing report totals");
    b.reconcile(pms, 1, rep);
    expect(rep.recon_rows == 1 && rep.recon_diff_rows == 1 && rep.recon[0].diff == -4, "clearing reconcile diff");

    char csv[96];
    ci_path(csv, sizeof csv, "ops_clear_ci.csv");
    expect(b.snapshot_csv(csv, 0), "clearing csv");
    std::FILE* cf = std::fopen(csv, "r");
    expect(cf != nullptr, "clearing csv reopen");
    if (cf) {
        char line[256];
        int lines = 0;
        while (std::fgets(line, sizeof line, cf)) ++lines;
        std::fclose(cf);
        expect(lines == 2, "clearing csv rows");
    }
    std::remove(csv);

    // Cash ledger, today/yesterday lots, close-today fees, dual valuation, NAV,
    // and the day history, following the fund-clearing reference model.
    pitlane::ClearingBook nb;
    nb.reset(9, pitlane::kEnvTest);
    nb.set_cash(1000000);
    nb.set_shares(1000000);
    pitlane::ClrCashMove dep = nb.deposit(50000, 0, 900);
    expect(dep.seq == 1 && dep.base_amount == 50000 && nb.cash_balance() == 1050000, "clearing deposit");
    pitlane::ClrCashMove wd = nb.withdraw(20000, 0, 901);
    expect(wd.seq == 2 && wd.base_amount == 20000 && nb.cash_balance() == 1030000, "clearing withdraw");
    pitlane::ClrCashMove over = nb.withdraw(99999999, 0, 902);
    expect(over.seq == 0 && nb.cash_balance() == 1030000, "clearing withdraw guard");
    pitlane::ClrCashMove moves[4]{};
    expect(nb.cash_moves_since(0, moves, 4) == 2 && moves[0].seq == 1 && moves[1].amount == 20000,
           "clearing cash ledger");
    expect(nb.cash_moves_since(1, moves, 4) == 1 && moves[0].seq == 2, "clearing cash ledger since");

    pitlane::ClrFillIn q{};
    q.name = 5;
    q.side = 1;
    q.qty = 10000;
    q.px = 1000000;
    q.fee_ppm = 100;
    q.close_fee_ppm = 100;
    q.close_today_fee_ppm = 300;
    q.margin_ppm = 100000;
    nb.apply(q, 1000);
    q.side = 2;
    q.qty = 4000;
    q.px = 1100000;
    pitlane::ClrFillOut qo = nb.apply(q, 1001);
    expect(qo.closed_today == 4000 && qo.closed_yday == 0, "clearing today lots close first");
    expect(qo.commission == 132 && qo.open_fee == 0, "clearing close today fee");

    pitlane::ClrMark sm[1]{};
    sm[0].name = 5;
    sm[0].last = 1200000;
    sm[0].settle = 1150000;
    pitlane::ClearingReport nrep{};
    nb.report(sm, 1, pitlane::kEnvTest, nrep);
    expect(nrep.row[0].long_today == 6000 && nrep.row[0].long_yday == 0, "clearing today qty");
    expect(nrep.unrealized == 120000 && nrep.unrealized_settle == 90000, "clearing dual unrealized");
    const int64_t pre = nrep.equity;
    nb.settle(2000, sm, 1);
    nb.report(sm, 1, pitlane::kEnvTest, nrep);
    expect(nb.realized_settle() == 130000, "clearing settled pnl");
    expect(nrep.row[0].long_yday == 6000 && nrep.row[0].long_today == 0, "clearing lots roll to yesterday");
    expect(nrep.unrealized == 120000 && nrep.unrealized_settle == 0, "clearing settle rebase");
    expect(nrep.equity == pre && nrep.today_pnl == 0, "clearing settle keeps equity");
    expect(nrep.cash_balance == 1030000 && nrep.total_pnl == nrep.equity - nrep.cash_balance,
           "clearing total pnl");
    expect(nrep.unit_nav == nrep.equity && nrep.peak_nav >= nrep.unit_nav, "clearing unit nav");
    expect(nb.day_count() == 1 && nb.day_at(0).day_seq == 1 && nb.day_at(0).settled == 90000 &&
               nb.day_at(0).realized == 40000,
           "clearing day history");
    expect(nrep.days == 1 && nrep.day[0].equity == pre && nrep.day[0].fees == 232, "clearing report days");

    q.side = 2;
    q.qty = 6000;
    q.px = 1150000;
    qo = nb.apply(q, 2001);
    expect(qo.realized == 90000 && qo.realized_settle == 0 && qo.closed_yday == 6000, "clearing yday close basis");

    // Foreign-currency rows convert to the base currency for the totals.
    pitlane::ClearingBook fx;
    fx.reset(11, pitlane::kEnvTest);
    fx.set_cash(1000000);
    const int usd = fx.ccy_id("USD");
    expect(usd > 0, "clearing ccy register");
    fx.set_rate(static_cast<uint8_t>(usd), 7000000);
    pitlane::ClrFillIn u{};
    u.name = 6;
    u.side = 1;
    u.qty = 10;
    u.px = 1000000;
    u.ccy = static_cast<uint8_t>(usd);
    fx.apply(u, 1);
    pitlane::ClrMark um[1]{};
    um[0].name = 6;
    um[0].last = 1100000;
    um[0].settle = 1100000;
    pitlane::ClearingReport frep{};
    fx.report(um, 1, pitlane::kEnvTest, frep);
    expect(frep.row[0].ccy == static_cast<uint8_t>(usd) && frep.row[0].fx_ppm == 7000000,
           "clearing row ccy");
    expect(frep.unrealized == 700 && frep.fx_missing == 0, "clearing fx conversion");

    pitlane::ClearingBoard board;
    expect(!board.read(rep) || rep.acct == 0, "clearing board empty");
    pitlane::ClearingReport pub{};
    pub.acct = 7;
    pub.realized = 12;
    board.publish(pub);
    pitlane::ClearingReport got{};
    for (int i = 0; i < 8 && !board.read(got); ++i) {
    }
    expect(got.acct == 7 && got.realized == 12, "clearing board roundtrip");
}

static void check_rms_short() {
    pitlane::Rms rms;
    pitlane::RmsLimits lim{};
    rms.set(lim);
    pitlane::Pms<8> pms;
    pitlane::PmsFill pf{};
    pf.acct = 1;
    pf.name = 1;
    pf.side = 1;
    pf.qty = 5;
    pf.px = 1000;
    expect(pms.apply(pf), "rms short pms open");

    pitlane::RmsReq r{};
    r.acct = 1;
    r.name = 1;
    r.side = 2;
    r.qty = 3;
    r.px = 1000;
    r.free_cash = 100000000;
    r.pos = pms.find(1, 1);
    r.allow_short = false;
    expect(rms.check(r) == pitlane::RmsCode::Ok, "rms sell covers long");
    r.qty = 6;
    expect(rms.check(r) == pitlane::RmsCode::Short, "rms sell over long");
    r.allow_short = true;
    expect(rms.check(r) == pitlane::RmsCode::Ok, "rms short product allows short");
    r.pos = nullptr;
    r.allow_short = false;
    expect(rms.check(r) == pitlane::RmsCode::Short, "rms short without position");
    expect(std::strcmp(pitlane::rms_code_name(pitlane::RmsCode::Short), "short not allowed") == 0,
           "rms short reason");
}

static void check_session_mode() {
    pitlane::OpsUserStore users;
    expect(users.add("risk1", "secret1", pitlane::kRoleRisk, false, nullptr), "session user");
    expect(pitlane::ops_has(pitlane::ops_role_perms(pitlane::kRoleRisk), pitlane::kPermClear), "risk has clearing");
    pitlane::OpsSessionStore sessions;
    const int idx = sessions.create(users.at(0), 1000, nullptr, pitlane::kEnvLive);
    expect(idx >= 0 && sessions.at(idx).mode == pitlane::kEnvLive, "session live mode");
    const int idx2 = sessions.create(users.at(0), 1000, nullptr);
    expect(idx2 >= 0 && sessions.at(idx2).mode == pitlane::kEnvTest, "session default test mode");
}

static void fill_spec(pitlane::StratSpec& s, const char* id) {
    std::snprintf(s.id, sizeof s.id, "%s", id);
    s.acct = 1;
    s.instrument = 1;
    s.kind = 2;
    s.window = 16;
    s.sign = 1;
    s.threshold_ppm = 10;
    s.qty = 1;
    s.cooldown_us = 1000000;
    s.env = 0;
}

static pitlane::StratCmd make_strat_cmd(uint8_t kind, const char* id, uint8_t env, uint64_t seq) {
    pitlane::StratCmd c{};
    c.seq = seq;
    c.kind = kind;
    c.env = env;
    std::snprintf(c.id, sizeof c.id, "%s", id);
    return c;
}

static void check_strategy_registry() {
    pitlane::StrategyRegistry reg;
    pitlane::StratSpec spec{};
    fill_spec(spec, "s1");
    const int slot = reg.apply_add(spec, 1000);
    expect(slot == 0 && reg.used(slot) && reg.state(slot) == pitlane::kStratIdle, "strat add");
    expect(reg.apply_add(spec, 1001) < 0, "strat duplicate rejected");
    expect(reg.find("s1") == slot && reg.find("nope") < 0, "strat find");

    pitlane::StratCmd c = make_strat_cmd(pitlane::kStratCmdStart, "s1", 0, 1);
    expect(reg.apply(c, 2000) == slot && reg.state(slot) == pitlane::kStratRunning, "strat start");
    c.kind = pitlane::kStratCmdPause;
    expect(reg.apply(c, 3000) == slot && reg.state(slot) == pitlane::kStratPaused, "strat pause");
    c.kind = pitlane::kStratCmdResume;
    expect(reg.apply(c, 4000) == slot && reg.state(slot) == pitlane::kStratRunning, "strat resume");
    c.kind = pitlane::kStratCmdStop;
    expect(reg.apply(c, 5000) == slot && reg.state(slot) == pitlane::kStratStopped, "strat stop");

    const pitlane::StratCmd queued = make_strat_cmd(pitlane::kStratCmdStart, "s1", 0, 42);
    expect(reg.submit(queued), "strat submit");
    pitlane::StratCmd taken{};
    expect(reg.take(taken) && taken.seq == 42 && taken.kind == queued.kind &&
               std::strcmp(taken.id, "s1") == 0,
           "strat take roundtrip");

    pitlane::StratRow row{};
    std::snprintf(row.id, sizeof row.id, "s2");
    row.state = pitlane::kStratRunning;
    row.kind = 2;
    row.window = 16;
    row.sign = -1;
    row.env = 0;
    row.acct = 2;
    row.instrument = 3;
    row.threshold_ppm = 77;
    row.qty = 5;
    row.factor = 123;
    row.position = -2;
    row.signals = 9;
    row.orders = 8;
    row.rejects = 1;
    row.realized = 40;
    row.unrealized = -7;
    row.last_sig_us = 111;
    row.started_us = 222;
    std::snprintf(row.note, sizeof row.note, "hello");
    reg.publish(1, row);
    pitlane::StratRow got{};
    expect(reg.read(1, got), "strat read");
    expect(std::strcmp(got.id, row.id) == 0 && got.state == row.state && got.kind == row.kind &&
               got.window == row.window && got.sign == row.sign && got.env == row.env &&
               got.acct == row.acct && got.instrument == row.instrument &&
               got.threshold_ppm == row.threshold_ppm && got.qty == row.qty &&
               got.factor == row.factor && got.position == row.position &&
               got.signals == row.signals && got.orders == row.orders &&
               got.rejects == row.rejects && got.realized == row.realized &&
               got.unrealized == row.unrealized && got.last_sig_us == row.last_sig_us &&
               got.started_us == row.started_us && std::strcmp(got.note, row.note) == 0,
           "strat row roundtrip");

    const pitlane::StratCmd wrong = make_strat_cmd(pitlane::kStratCmdStart, "s1", 1, 2);
    expect(reg.apply(wrong, 6000) < 0 && reg.state(slot) == pitlane::kStratStopped, "strat env mismatch");

    for (int i = 2; i <= pitlane::StrategyRegistry::kMaxStrategies; ++i) {
        pitlane::StratSpec extra{};
        char id[24];
        std::snprintf(id, sizeof id, "s%d", i);
        fill_spec(extra, id);
        expect(reg.apply_add(extra, 7000 + i) >= 0, "strat fill slot");
    }
    expect(reg.active_count() == pitlane::StrategyRegistry::kMaxStrategies, "strat full count");

    pitlane::StratSpec over{};
    fill_spec(over, "s9");
    expect(reg.apply_add(over, 9000) < 0, "strat full rejected");
}

static void check_group_risk() {
    pitlane::GroupRiskBook b{};
    b.clear(0);
    pitlane::GroupCaps caps{};
    caps.max_gross = 1000;
    caps.max_net = 600;
    caps.max_acct = 400;
    expect(b.set_caps(0, caps), "group caps");
    expect(b.add(0, 1, 5, 100), "group add long");
    expect(b.add(0, 2, -2, 100), "group add short");
    expect(!b.add(0, 33, 1, 1), "group bad account");
    expect(!b.add(9, 1, 1, 1), "group bad group");
    b.evaluate(1000);
    const pitlane::GroupRow& r = b.row(0);
    expect(r.gross == 700, "group gross");
    expect(r.net == 300, "group net");
    expect(r.largest == 500, "group largest");
    expect(r.accounts == 2, "group accounts");
    expect((r.breach & pitlane::kGroupAcct) != 0, "group account breach");
    expect(b.blocked(0), "group blocked");
    expect(b.gross_headroom(0) == 300, "group gross headroom");
    expect(b.net_headroom(0) == 300, "group net headroom");
    b.halt(0, true);
    b.evaluate(2000);
    expect((b.row(0).breach & pitlane::kGroupHalted) != 0, "group halt keeps");
    b.halt(0, false);
    b.evaluate(3000);
    expect((b.row(0).breach & pitlane::kGroupHalted) == 0, "group halt clears");

    pitlane::GroupRiskBook z{};
    z.clear(0);
    z.add(0, 1, 100, 1000000);
    z.evaluate(0);
    expect(!z.blocked(0), "group caps off");
    expect(z.gross_headroom(0) == INT64_MAX, "group headroom off");
    expect(z.row(0).gross > 0, "group gross large");
}

static void check_parity() {
    // --- income and expense ledger ---------------------------------------
    {
        pitlane::ClearingBook b;
        b.reset(1, pitlane::kEnvTest);
        b.set_cash(1000000);
        b.set_shares(1000000);
        b.accrue_expense(pitlane::kClrMgmtFee, 100, 1000);
        b.accrue_expense(pitlane::kClrCustodyFee, 40, 1001);
        b.accrue_expense(pitlane::kClrMgmtFee, -40, 1002);  // reversal
        b.accrue_interest(25, 1003);
        pitlane::ClearingReport r{};
        b.report(nullptr, 0, pitlane::kEnvTest, r);
        expect(r.expense == 125 && r.mgmt_fee == 60 && r.custody_fee == 40, "accrual buckets");
        expect(r.fin_interest == 25 && r.income == 0, "accrual interest");
        expect(r.cash_balance == 999875 && r.equity == r.cash_balance, "accrual cash");
        expect(r.today_pnl == -125 && r.total_pnl == -125, "accrual pnl");
        b.deposit(500, 0, 1004);
        b.withdraw(200, 0, 1005);
        b.report(nullptr, 0, pitlane::kEnvTest, r);
        expect(r.cash_balance == 1000175 && r.net_flow == 300, "flow cash");
        expect(r.today_pnl == -125 && r.total_pnl == -125, "flow excluded from pnl");

        char csv[96];
        ci_path(csv, sizeof csv, "ops_parity_ci.csv");
        expect(b.snapshot_csv(csv, 0), "parity csv");
        std::FILE* cf = std::fopen(csv, "r");
        expect(cf != nullptr, "parity csv reopen");
        if (cf) {
            char line[512];
            expect(std::fgets(line, sizeof line, cf) != nullptr, "parity csv header");
            expect(std::strstr(line, "cum_nav,income,expense,net_flow") != nullptr, "parity csv columns");
            std::fclose(cf);
        }
        std::remove(csv);
    }

    // --- cash dividend, withholding, idempotency, shorts ------------------
    {
        pitlane::ClearingBook b;
        b.reset(3, pitlane::kEnvTest);
        b.set_cash(0);
        b.set_shares(1000000);
        pitlane::ClrFillIn f{};
        f.name = 10;
        f.side = 1;
        f.qty = 1000;
        f.px = 100000;
        b.apply(f, 1);
        pitlane::ClrCorp c{};
        c.name = 10;
        c.event_id = 7;
        c.kind = pitlane::kClrCashDiv;
        c.per10_cash = 5000;
        c.tax_ppm = 100000;
        const pitlane::ClrCorpOut o = b.apply_corp(c, 2);
        expect(o.applied && o.cash == 45 && o.tax == 5, "cash dividend net of tax");
        expect(b.income() == 45 && b.cash_balance() == 45, "cash dividend books income");
        const pitlane::ClrCorpOut again = b.apply_corp(c, 3);
        expect(!again.applied && b.income() == 45 && b.cash_balance() == 45, "cash dividend idempotent");
        expect(b.distributions_ppm() == 50, "dividend distribution ppm");
        expect(b.cumulative_nav() == b.unit_nav() + b.distributions_ppm(), "cumulative nav");

        pitlane::ClearingBook s;
        s.reset(4, pitlane::kEnvTest);
        s.set_cash(1000);
        s.set_shares(1000);
        pitlane::ClrFillIn g{};
        g.name = 11;
        g.side = 2;
        g.qty = 100;
        g.px = 100000;
        g.allow_short = true;
        s.apply(g, 1);
        pitlane::ClrCorp sc = c;
        sc.name = 11;
        const pitlane::ClrCorpOut so = s.apply_corp(sc, 2);
        expect(so.applied && so.cash == -5 && so.tax == 0, "short dividend payable");
        expect(s.income() == -5 && s.cash_balance() == 995, "short dividend cash");
    }

    // --- stock dividend, short block, split and merge ---------------------
    {
        pitlane::ClearingBook b;
        b.reset(5, pitlane::kEnvTest);
        b.set_cash(0);
        b.set_shares(1000);
        pitlane::ClrFillIn f{};
        f.name = 12;
        f.side = 1;
        f.qty = 1000;
        f.px = 100000;
        b.apply(f, 1);
        pitlane::ClrCorp c{};
        c.name = 12;
        c.event_id = 1;
        c.kind = pitlane::kClrStockDiv;
        c.per10_stock = 3;
        const pitlane::ClrCorpOut o = b.apply_corp(c, 2);
        expect(o.applied && o.qty_added == 300 && b.net_qty(12) == 1300, "stock dividend shares");
        pitlane::ClrMark m[1]{};
        m[0].name = 12;
        m[0].last = 100000;
        pitlane::ClearingReport r{};
        b.report(m, 1, pitlane::kEnvTest, r);
        expect(r.unrealized == 3000, "stock dividend zero cost");

        pitlane::ClrCorp sp{};
        sp.name = 12;
        sp.event_id = 2;
        sp.kind = pitlane::kClrSplit;
        sp.num = 2;
        sp.den = 1;
        const pitlane::ClrCorpOut so = b.apply_corp(sp, 3);
        expect(so.applied && so.qty_added == 1300 && b.net_qty(12) == 2600, "split doubles quantity");
        m[0].last = 50000;
        b.report(m, 1, pitlane::kEnvTest, r);
        expect(r.unrealized == 3000, "split keeps value");
        sp.event_id = 3;
        sp.num = 0;
        const pitlane::ClrCorpOut bad = b.apply_corp(sp, 4);
        expect(!bad.applied && b.net_qty(12) == 2600, "invalid split rejected");
        sp.num = 1;
        sp.den = 2;
        sp.event_id = 4;
        sp.kind = pitlane::kClrMerge;
        const pitlane::ClrCorpOut mo = b.apply_corp(sp, 5);
        expect(mo.applied && mo.qty_removed == 1300 && b.net_qty(12) == 1300, "merge halves quantity");
        m[0].last = 100000;
        b.report(m, 1, pitlane::kEnvTest, r);
        expect(r.row[0].avg_long == 76923, "merge keeps cost basis");
        const pitlane::ClrCorpOut replay = b.apply_corp(sp, 6);
        expect(!replay.applied && b.net_qty(12) == 1300, "merge replay idempotent");
        sp.event_id = 7;
        sp.num = 2;
        sp.den = 1;
        expect(!b.apply_corp(sp, 7).applied && b.net_qty(12) == 1300, "merge requires a shrinking ratio");

        pitlane::ClearingBook shorted;
        shorted.reset(6, pitlane::kEnvTest);
        pitlane::ClrFillIn sh{};
        sh.name = 13;
        sh.side = 2;
        sh.qty = 100;
        sh.px = 100000;
        sh.allow_short = true;
        shorted.apply(sh, 1);
        pitlane::ClrCorp sd{};
        sd.name = 13;
        sd.event_id = 9;
        sd.kind = pitlane::kClrStockDiv;
        sd.per10_stock = 2;
        const pitlane::ClrCorpOut blocked = shorted.apply_corp(sd, 2);
        expect(!blocked.applied && blocked.short_blocked && shorted.net_qty(13) == -100, "stock dividend blocks short");

        pitlane::ClearingBook round;
        round.reset(7, pitlane::kEnvTest);
        pitlane::ClrFillIn rf{};
        rf.name = 14;
        rf.side = 1;
        rf.qty = 10;
        rf.px = 100000;
        round.apply(rf, 1);
        pitlane::ClrCorp rev{};
        rev.name = 14;
        rev.event_id = 1;
        rev.kind = pitlane::kClrSplit;
        rev.num = 1;
        rev.den = 3;
        expect(round.apply_corp(rev, 2).applied && round.net_qty(14) == 3, "split rounds down");
    }

    // --- shares, unit NAV, and period sums --------------------------------
    {
        pitlane::ClearingBook b;
        b.reset(8, pitlane::kEnvTest);
        b.set_cash(1000000);
        b.set_shares(1000000);
        pitlane::ClearingReport r{};
        b.report(nullptr, 0, pitlane::kEnvTest, r);
        expect(r.unit_nav == 1000000, "unit nav at par");
        expect(b.subscribe(500, 1), "subscribe accepted");
        b.report(nullptr, 0, pitlane::kEnvTest, r);
        expect(r.cash_balance == 1000500 && r.shares == 1000500 && r.shares_in == 500, "subscribe books shares");
        expect(r.today_pnl == 0 && r.net_flow == 500, "subscribe moves cash");
        expect(!b.redeem(2000000, 2), "redeem above balance rejected");
        expect(b.redeem(500, 3), "redeem accepted");
        b.report(nullptr, 0, pitlane::kEnvTest, r);
        expect(r.cash_balance == 1000000 && r.shares == 1000000 && r.shares_out == 500, "redeem retires shares");
        expect(r.today_pnl == 0 && r.net_flow == 0, "redeem moves cash");

        pitlane::ClearingBook p;
        p.reset(9, pitlane::kEnvTest);
        p.set_cash(1000000);
        p.set_shares(1000000);
        pitlane::ClrFillIn f{};
        f.name = 20;
        f.side = 1;
        f.qty = 1000;
        f.px = 100000;
        p.apply(f, 1);
        pitlane::ClrMark m[1]{};
        m[0].name = 20;
        m[0].last = 110000;
        m[0].settle = 110000;
        p.report(m, 1, pitlane::kEnvTest, r);
        const int64_t now = 1767225600000000ll;  // 2026-01-01T00:00:00Z
        p.settle(now, m, 1);
        pitlane::ClearingReport pr{};
        p.report(m, 1, pitlane::kEnvTest, pr);
        expect(pr.days == 1 && pr.day[0].pnl == 1000, "day pnl recorded");
        expect(pr.today_pnl == 0, "settled day is flat");
        const pitlane::ClrPeriod per = pitlane::clr_period(pr, now + 1000);
        expect(per.today == 0 && per.d7 == 1000 && per.d30 == 1000 && per.ytd == 1000, "period sums");
        expect(pitlane::clr_year_of(now) == 2026 && pitlane::clr_year_of(now - 1000000) == 2025,
               "period year boundary");
    }

    // --- custodian statements and three-way reconciliation -----------------
    {
        char pa[128];
        char pb[128];
        ci_path(pa, sizeof pa, "ops_stmt_a.csv");
        ci_path(pb, sizeof pb, "ops_stmt_b.csv");
        std::FILE* f = std::fopen(pa, "w");
        expect(f != nullptr, "statement write");
        if (f) {
            std::fprintf(f, "name,qty,equity,realized,unrealized\r\n# comment\r\n1,100,25,10,20\r\n2,40\r\n\r\n");
            std::fclose(f);
        }
        pitlane::StmtBook a;
        expect(a.load_csv(pa) && a.count() == 2, "statement parse rows");
        const pitlane::StmtRow* r1 = a.find(1);
        expect(r1 && r1->qty == 100 && r1->equity == 25 && r1->unrealized == 20, "statement row values");
        const pitlane::StmtRow* r2 = a.find(2);
        expect(r2 && r2->qty == 40 && r2->equity == 0, "statement optional money columns");

        pitlane::StmtBook b;
        expect(!b.load_csv(pb), "statement missing file");

        pitlane::ClearingReport rep{};
        rep.rows = 2;
        rep.row[0].name = 1;
        rep.row[0].long_qty = 100;
        rep.row[0].realized = 10;
        rep.row[0].unrealized = 20;
        rep.row[0].fees = 5;
        rep.row[1].name = 2;
        rep.row[1].long_qty = 50;

        pitlane::ReconOut out{};
        expect(pitlane::recon_three_way(rep, &a, nullptr, 0, 0, out), "recon runs");
        expect(out.count == 2 && out.qty_breaks == 1 && out.money_breaks == 0, "recon qty break");
        expect(out.line[0].present == 3 && !out.line[0].qty_bad && !out.line[0].money_bad, "recon clean name");
        expect(out.line[1].dqty[0] == 10 && out.line[1].qty_bad, "recon difference value");
        expect(out.line[1].dqty[1] == pitlane::kReconMissing, "recon missing source");

        pitlane::ReconOut tol{};
        pitlane::recon_three_way(rep, &a, nullptr, 10, 0, tol);
        expect(tol.qty_breaks == 0, "recon inclusive tolerance");

        // Statement B adds a name and disagrees on equity for name 1.
        f = std::fopen(pb, "w");
        if (f) {
            std::fprintf(f, "name,qty,equity,realized,unrealized\n1,100,30,10,20\n3,7,70,0,70\n");
            std::fclose(f);
        }
        pitlane::StmtBook b2;
        expect(b2.load_csv(pb) && b2.count() == 2, "statement b parse");
        pitlane::ReconOut three{};
        pitlane::recon_three_way(rep, &a, &b2, 0, 0, three);
        expect(three.count == 3, "recon union of names");
        expect(three.line[0].present == 7 && three.line[0].money_bad, "recon money break");
        expect(three.line[0].dequity[1] == -5, "recon internal to b");
        expect(three.line[2].present == 4 && !three.line[2].qty_bad, "recon b-only name");

        char pc[160];
        ci_path(pc, sizeof pc, "ops_recon_ci.csv");
        expect(pitlane::recon_csv(three, pc), "recon csv");
        std::FILE* cf = std::fopen(pc, "r");
        expect(cf != nullptr, "recon csv reopen");
        if (cf) {
            char line[512];
            expect(std::fgets(line, sizeof line, cf) != nullptr &&
                       std::strncmp(line, "name,present,qty_i", 18) == 0,
                   "recon csv header");
            std::fclose(cf);
        }
        std::remove(pc);
        std::remove(pa);
        std::remove(pb);
    }

    // --- host health ------------------------------------------------------
    {
        pitlane::HealthSample hs{};
        pitlane::HealthLimits lim{};
        pitlane::HealthState st{};
        hs.cpu_ppm = 800000;
        pitlane::health_eval(hs, lim, st);
        expect(st.level == 1 && (st.bits & pitlane::kHealthCpu) != 0, "health warn at eighty percent");
        hs.cpu_ppm = 900000;
        pitlane::health_eval(hs, lim, st);
        expect(st.level == 2, "health critical at limit");
        hs.cpu_ppm = 0;
        hs.mem_ppm = 950000;
        pitlane::health_eval(hs, lim, st);
        expect(st.level == 2 && (st.bits & pitlane::kHealthMem) != 0 && (st.bits & pitlane::kHealthCpu) == 0,
               "health bits follow the sample");
        hs = pitlane::HealthSample{};
        hs.fds = 4096;
        pitlane::health_eval(hs, lim, st);
        expect(st.level == 2 && (st.bits & pitlane::kHealthFds) != 0, "health fd limit");
        hs = pitlane::HealthSample{};
        hs.heartbeat_us = 1600000;
        pitlane::health_eval(hs, lim, st);
        expect(st.level == 1 && (st.bits & pitlane::kHealthHeartbeat) != 0, "health heartbeat warn");
        hs.heartbeat_us = 2000000;
        pitlane::health_eval(hs, lim, st);
        expect(st.level == 2, "health heartbeat critical");
        hs = pitlane::HealthSample{};
        pitlane::health_eval(hs, lim, st);
        expect(st.level == 0 && st.bits == 0, "health recovers");

        pitlane::HealthSample live{};
        pitlane::health_beat(1000000);
        expect(pitlane::health_sample(live, 2000000), "health sample reads proc");
        expect(live.threads >= 1 && live.fds > 0, "health sample counters");
        expect(live.mem_ppm >= 0 && live.mem_ppm <= 1000000 && live.disk_used_ppm >= 0 &&
                   live.disk_used_ppm <= 1000000,
               "health sample ratios");
        pitlane::HealthSample live2{};
        expect(pitlane::health_sample(live2, 3000000), "health sample second");
        expect(live2.heartbeat_us == 2000000, "health heartbeat age");
        expect(live2.uptime_s >= 0, "health uptime");
    }
}

static pitlane::SeatSpec seat_of(uint32_t id, const char* name, const char* channel, int32_t cap) {
    pitlane::SeatSpec s{};
    s.id = id;
    std::snprintf(s.name, sizeof s.name, "%s", name);
    std::snprintf(s.channel, sizeof s.channel, "%s", channel);
    std::snprintf(s.exchange, sizeof s.exchange, "SHFE");
    s.max_orders_per_sec = cap;
    return s;
}

static void check_seats() {
    pitlane::SeatRegistry reg;
    expect(reg.add(seat_of(1, "s1", "ch-a", 2)) == 0, "seat add");
    expect(reg.add(seat_of(1, "dup", "ch-a", 0)) == -1, "seat dup");
    expect(reg.add(seat_of(2, "s2", "ch-a", 2)) == 1, "seat add 2");
    expect(reg.channels() == 1 && reg.channel_seats(0) == 2, "seat channel");
    expect(reg.start(1, 10) && reg.start(2, 10), "seat start");

    const int a = reg.pick("ch-a", 20);
    expect(a == 0 || a == 1, "seat pick");
    reg.note_order(a, true, 20);
    const int b = reg.pick("ch-a", 21);
    expect(b >= 0 && b != a, "seat pick least loaded");

    expect(reg.fault(2, 30), "seat fault");
    expect(reg.pick("ch-a", 31) == 0, "seat skip fault");
    expect(reg.pick("no-such", 32) == -1, "seat unknown channel");

    reg.tick_window();
    reg.note_order(0, false, 40);
    expect(reg.at(0).window_orders == 1 && reg.at(0).rejects == 1, "seat counters");
}

static void check_logsearch() {
    pitlane::LogIndex log;
    log.add(1000, pitlane::kLogInfo, pitlane::kLogOrder, "order accepted rb2601");
    log.add(2000, pitlane::kLogWarn, pitlane::kLogRisk, "risk reject: band");
    log.add(3000, pitlane::kLogError, pitlane::kLogOrder, "ORDER timeout tag=7");
    expect(log.count() == 3, "log count");

    pitlane::LogLine out[8]{};
    expect(log.search("order", 0, pitlane::kLogDebug, 0xff, out, 8) == 2, "log ci search");
    expect(out[0].ts_us == 3000 && out[1].ts_us == 1000, "log newest first");
    expect(log.search("ORDER", 1500, pitlane::kLogDebug, 0xff, out, 8) == 1, "log since");
    expect(log.search("", 0, pitlane::kLogWarn, 0xff, out, 8) == 2, "log level");
    expect(log.search("", 0, pitlane::kLogDebug, pitlane::kLogRisk, out, 8) == 1, "log kind");
    expect(log.search("missing", 0, pitlane::kLogDebug, 0xff, out, 8) == 0, "log miss");

    char dump[512];
    const int n = log.dump(dump, sizeof dump);
    expect(n > 0 && std::strstr(dump, "risk reject") != nullptr, "log dump");
}

static void check_canary() {
    pitlane::Canary c;
    pitlane::CanaryPlan p{};
    p.id = 7;
    std::snprintf(p.version, sizeof p.version, "v2");
    p.pct_ppm = 500000;
    p.min_samples = 5;
    p.max_reject_ppm = 50000;
    p.max_slip = 2;
    expect(c.plan(p), "canary plan");
    expect(c.state() == pitlane::kCanaryLive, "canary live");
    const bool b1 = c.pick(12345);
    expect(b1 == c.pick(12345), "canary stable bucket");

    for (int i = 0; i < 5; ++i) c.note(true, true, 1);
    expect(c.evaluate(), "canary gate ok");
    expect(c.promote() && c.state() == pitlane::kCanaryPromoted, "canary promote");
    expect(c.pick(99), "canary promoted routes all");

    pitlane::Canary r;
    expect(r.plan(p), "canary2 plan");
    for (int i = 0; i < 5; ++i) r.note(true, false, 5);
    expect(!r.evaluate() && !r.promote(), "canary gate reject");
    expect(r.rollback() && r.state() == pitlane::kCanaryRolledBack, "canary rollback");
}

static void check_replayctl() {
    pitlane::ReplayCtl rc;
    rc.load(0, 1000, 2000000);
    expect(rc.play(100), "replay play");
    int64_t tape = 0;
    expect(rc.advance(300, tape) && tape == 400, "replay 2x");
    expect(rc.pause() && rc.state() == pitlane::kReplayPaused, "replay pause");
    expect(!rc.advance(400, tape), "replay paused");
    expect(rc.step(100) && rc.cursor() == 500, "replay step");
    expect(rc.seek(1000, 0) && rc.report().progress_ppm == 1000000, "replay seek end");
    expect(rc.set_speed(500000) && rc.speed() == 500000, "replay speed");
    expect(rc.play(200), "replay play end");
    expect(rc.advance(300, tape) && tape == 1000 && rc.state() == pitlane::kReplayDone, "replay done");
}

static void check_nav_report() {
    using namespace pitlane;
    NavTrack<8> track;
    expect(!track.push(RptPoint{}), "report rejects a zero stamp");
    RptPoint p{};
    p.ts_us = 1000000;
    p.nav_ppm = 1000000;
    p.benchmark_ppm = 1000000;
    p.equity = 100000;
    p.fees = 10;
    expect(track.push(p), "report first point");
    p.ts_us = 86401000000;
    p.nav_ppm = 1100000;
    p.benchmark_ppm = 1050000;
    expect(track.push(p), "report second point");
    p.ts_us = 172801000000;
    p.nav_ppm = 990000;
    p.benchmark_ppm = 1060000;
    p.fees = 40;
    expect(track.push(p), "report third point");
    expect(track.size() == 3, "report retains three points");

    // A repeat timestamp replaces the tail instead of double counting.
    p.nav_ppm = 990000;
    expect(track.push(p) && track.size() == 3, "report replaces a repeated stamp");

    RptStats s{};
    expect(report_fold(track, 0, s), "report folds");
    expect(s.points == 3 && s.intervals == 2, "report interval count");
    expect(s.return_ppm == -10000, "report return");
    expect(s.benchmark_ppm == 60000, "report benchmark");
    expect(s.excess_ppm == -70000, "report excess");
    expect(s.max_drawdown_ppm == 100000, "report drawdown");
    expect(s.realtime_drawdown_ppm == 100000, "report live drawdown");
    expect(s.win_ppm == 500000, "report hit rate");
    expect(s.best_ppm == 100000 && s.worst_ppm == -100000, "report extremes");
    expect(s.volatility_ppm == 100000, "report volatility");
    expect(s.mean_ppm == 0 && s.sharpe_ppm == 0, "report flat mean");
    expect(s.tracking_ppm == 79761, "report tracking error");
    expect(s.info_ppm < 0, "report negative information ratio");
    expect(s.annual_ppm == -1825000, "report annualised");
    expect(s.fees == 40, "report fees");

    // Period starts: 2026-11-17 12:00 UTC.
    const int64_t noon = 1794916800ll * 1000000;
    expect(rpt_since(static_cast<uint8_t>(RptPeriod::Daily), noon) == 1794873600ll * 1000000, "daily start");
    expect(rpt_since(static_cast<uint8_t>(RptPeriod::Weekly), noon) == 1794787200ll * 1000000, "weekly start");
    expect(rpt_since(static_cast<uint8_t>(RptPeriod::Monthly), noon) == 1793491200ll * 1000000,
           "monthly start");
    expect(rpt_since(static_cast<uint8_t>(RptPeriod::Yearly), noon) == 1767225600ll * 1000000, "yearly start");

    RptStats empty{};
    expect(!report_fold(track, noon + 1000000, empty), "report before the first point");
    expect(std::strcmp(rpt_period_name(static_cast<uint8_t>(RptPeriod::Weekly)), "weekly") == 0, "period name");
}

int main() {
    check_status();
    check_channel();
    check_control();
    check_risk_guard();
    check_http();
    check_host();
    check_sha();
    check_auth();
    check_gateway();
    check_series();
    check_accounts();
    check_instruments();
    check_instrument_capacity();
    check_gateway_env();
    check_alarms();
    check_rms_short();
    check_clearing();
    check_parity();
    check_session_mode();
    check_strategy_registry();
    check_group_risk();
    check_seats();
    check_logsearch();
    check_canary();
    check_replayctl();
    check_nav_report();
    if (g_fails == 0) std::printf("ops: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
