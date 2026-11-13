// High-performance SimNow 7×24 closed-loop example.
// MD callback thread: Quote publish → strategy → Intent → TdClient::insert (IOC).
// Measures local / gate / roundtrip T2T with TSC conversion.

#include "md_engine.hpp"
#include "td_client.hpp"
#include "pitlane/book/venue.hpp"
#include "pitlane/desk/center.hpp"
#include "pitlane/desk/session.hpp"
#include "pitlane/desk/tape_md.hpp"
#include "pitlane/util/wirelog.hpp"
#include "pitlane/desk/ids.hpp"
#include "pitlane/desk/region.hpp"
#include "pitlane/desk/td_engine.hpp"
#include "pitlane/host/affinity.hpp"
#include "pitlane/time/cycle.hpp"
#include "pitlane/util/spin.hpp"
#include "pitlane/version.hpp"

#include <atomic>
#include <cerrno>
#include <new>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct T2TSample {
    uint64_t local_ns;
    uint64_t gate_ns;
    uint64_t roundtrip_ns;
};

struct App {
    static constexpr int kSamples = 4096;

    pitlane::StratCfg cfg{};
    pitlane::CtpTdClient* td{nullptr};
    pitlane::Desk* desk{nullptr};
    pitlane::CycleScale scale{};
    pitlane::IdMint mint{1, 1};

    T2TSample samples[kSamples]{};
    std::atomic<int> n_samples{0};
    std::atomic<int> live{0};
    std::atomic<int64_t> last_order_us{0};
    std::atomic<bool> done{false};
    std::atomic<bool> stop{false};
};

App* g_app = nullptr;

void on_sig(int) {
    if (g_app) g_app->stop.store(true, std::memory_order_release);
}

int64_t wall_us() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000ll + ts.tv_nsec / 1000;
}

uint64_t to_ns(const pitlane::CycleScale& scale, uint64_t cycles) noexcept {
    uint64_t ns = 0;
    if (!pitlane::scale_ns(scale, cycles, ns)) return 0;
    return ns;
}

void on_accept(void* user, const pitlane::LiveOrder& lo, int64_t, uint64_t roundtrip_ns) {
    auto* app = static_cast<App*>(user);
    const int i = app->n_samples.fetch_add(1, std::memory_order_relaxed);
    if (i < App::kSamples) {
        app->samples[i] = T2TSample{lo.local_ns, lo.gate_ns, roundtrip_ns};
    }
    app->live.fetch_sub(1, std::memory_order_relaxed);
    if (app->cfg.t2t_samples > 0 && i + 1 >= app->cfg.t2t_samples) {
        app->done.store(true, std::memory_order_release);
        app->stop.store(true, std::memory_order_release);
    }
}

void on_tick_hot(void* user, uint32_t name, const pitlane::Quote& q, uint64_t t0_cycles) {
    auto* app = static_cast<App*>(user);
    if (app->stop.load(std::memory_order_relaxed)) return;
    if (!app->td || !app->td->ready()) return;
    if (q.bid_px <= 0 || q.ask_px < q.bid_px || q.ask_qty <= 0) return;
    const int64_t spread = q.ask_px - q.bid_px;
    if (spread <= 0 || spread > app->cfg.max_spread_x10000) return;
    if (q.ask_px > app->cfg.buy_px_x10000) return;

    const int64_t now = wall_us();
    int64_t last = app->last_order_us.load(std::memory_order_relaxed);
    if (now - last < app->cfg.min_order_gap_us) return;
    if (!app->last_order_us.compare_exchange_weak(last, now, std::memory_order_relaxed)) return;
    if (app->live.load(std::memory_order_relaxed) >= app->cfg.max_live_orders) return;

    time_t sec = static_cast<time_t>(now / 1000000);
    std::tm tm{};
    localtime_r(&sec, &tm);
    const int sod = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    const uint64_t tag = app->mint.next(tm.tm_yday, sod);

    pitlane::Intent in{};
    in.tag = tag;
    in.name = name;
    in.acct = 1;
    in.px = q.ask_px;
    in.qty = app->cfg.order_qty;
    in.stamp_us = now;
    in.side = static_cast<uint8_t>(pitlane::Side::Buy);
    in.act = static_cast<uint8_t>(pitlane::Act::Enter);
    in.style = static_cast<uint8_t>(pitlane::Style::Immediate);

    if (!app->desk->intents.publish(in, 0)) return;

    uint64_t t_intent = 0;
    (void)pitlane::cycles_ordered(t_intent);
    const uint64_t local_ns = to_ns(app->scale, t_intent > t0_cycles ? t_intent - t0_cycles : 0);
    app->td->note_t0(tag, t0_cycles, now, local_ns);
    app->live.fetch_add(1, std::memory_order_relaxed);

    // Bypass TdEngine drain: insert immediately on MD thread for min gate latency.
    if (app->td->insert(in) != 0) {
        app->live.fetch_sub(1, std::memory_order_relaxed);
    }
}

bool warm_scale(pitlane::CycleScale& scale) {
    uint64_t c0 = 0, c1 = 0;
    timespec a{}, b{};
    if (!pitlane::cycles_ordered(c0) || clock_gettime(CLOCK_MONOTONIC, &a) != 0) return false;
    timespec deadline = a;
    deadline.tv_nsec += 20000000;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    for (;;) {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) break;
        pitlane::cpu_relax();
    }
    if (clock_gettime(CLOCK_MONOTONIC, &b) != 0 || !pitlane::cycles_ordered(c1) || c1 <= c0) return false;
    uint64_t ns = 0;
    if (b.tv_nsec >= a.tv_nsec)
        ns = static_cast<uint64_t>(b.tv_sec - a.tv_sec) * 1000000000ull + static_cast<uint64_t>(b.tv_nsec - a.tv_nsec);
    else
        ns = static_cast<uint64_t>(b.tv_sec - a.tv_sec - 1) * 1000000000ull +
             static_cast<uint64_t>(1000000000L + b.tv_nsec - a.tv_nsec);
    return pitlane::scale_set(scale, c1 - c0, ns);
}

void summarize(App& app) {
    const int n = app.n_samples.load(std::memory_order_acquire);
    const int use = n < app.cfg.t2t_samples || app.cfg.t2t_samples <= 0 ? n : app.cfg.t2t_samples;
    if (use <= 0) {
        std::printf("no T2T samples\n");
        return;
    }
    uint64_t local[App::kSamples];
    uint64_t gate[App::kSamples];
    uint64_t rt[App::kSamples];
    int nl = 0, ng = 0, nr = 0;
    for (int i = 0; i < use && i < App::kSamples; ++i) {
        if (app.samples[i].local_ns) local[nl++] = app.samples[i].local_ns;
        if (app.samples[i].gate_ns) gate[ng++] = app.samples[i].gate_ns;
        if (app.samples[i].roundtrip_ns) rt[nr++] = app.samples[i].roundtrip_ns;
    }
    auto sortn = [](uint64_t* a, int n) {
        for (int i = 1; i < n; ++i) {
            uint64_t v = a[i];
            int j = i;
            while (j > 0 && a[j - 1] > v) {
                a[j] = a[j - 1];
                --j;
            }
            a[j] = v;
        }
    };
    auto pct = [&](uint64_t* a, int n, int p) -> uint64_t {
        if (n <= 0) return 0;
        sortn(a, n);
        return a[(n - 1) * p / 100];
    };
    std::printf("\n=== pitlane %s simnow T2T (hot MD→insert) ===\n", pitlane::kVersion);
    std::printf("samples=%d\n", use);
    if (nl)
        std::printf("local_path_ns p50=%llu p99=%llu\n", static_cast<unsigned long long>(pct(local, nl, 50)),
                    static_cast<unsigned long long>(pct(local, nl, 99)));
    if (ng)
        std::printf("gate_path_ns  p50=%llu p99=%llu\n", static_cast<unsigned long long>(pct(gate, ng, 50)),
                    static_cast<unsigned long long>(pct(gate, ng, 99)));
    if (nr)
        std::printf("roundtrip_ns  p50=%llu p99=%llu\n", static_cast<unsigned long long>(pct(rt, nr, 50)),
                    static_cast<unsigned long long>(pct(rt, nr, 99)));
    std::printf("============================================\n");
}

} // namespace

bool ensure_dir(const char* path) {
    if (!path || !path[0]) return false;
    char tmp[512];
    std::snprintf(tmp, sizeof tmp, "%s", path);
    const std::size_t n = std::strlen(tmp);
    if (n == 0) return false;
    if (tmp[n - 1] == '/') tmp[n - 1] = 0;
    for (char* p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        ::mkdir(tmp, 0755);
        *p = '/';
    }
    return ::mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

int run_paper(const pitlane::Account& acct, const pitlane::StratCfg& cfg) {
    if (!ensure_dir(acct.work_dir.c_str())) return 2;
    pitlane::WireLog wlog(acct.log_path.c_str(), 3);
    wlog.log(2, pitlane::lit("simnow paper center"));

    ::unlink(acct.desk_path.c_str());
    pitlane::MapOpt opt{};
    opt.hugepage = 0;
    opt.lock = 0;
    opt.prefault = 1;
    pitlane::MapInfo map{};
    if (pitlane::map_create(acct.desk_path.c_str(), pitlane::desk_bytes(), &map, opt) != 0) return 1;
    pitlane::Desk* desk = pitlane::desk_view(&map, 1);
    if (!desk) return 1;

    pitlane::CycleScale scale{};
    if (!warm_scale(scale)) return 1;

    constexpr int kTape = 32;
    pitlane::Quote tape[kTape]{};
    for (int i = 0; i < kTape; ++i) {
        tape[i].name = 1;
        tape[i].bid_px = 1000000;
        tape[i].ask_px = 1000200;
        tape[i].last_px = 1000100;
        tape[i].bid_qty = 8;
        tape[i].ask_qty = 8;
        tape[i].local_us = 1000 + i;
    }
    pitlane::TapeMd md;
    md.src = tape;
    md.n = kTape;
    md.burst = 1;
    md.quote_mask = (1ull << pitlane::kBookReader) | (1ull << pitlane::kLoopReader);

    struct Hot : pitlane::StratEngine {
        pitlane::StratCfg cfg{};
        pitlane::CycleScale scale{};
        pitlane::IdMint mint{1, 1};
        int orders{0};
        int fills{0};
        int live{0};
        int64_t last_us{0};
        uint64_t t0[64]{};
        uint64_t local_ns[64]{};
        uint64_t round_ns[64]{};
        uint64_t tags[64]{};

        void on_quote(const pitlane::Quote& q) override {
            if (cfg.t2t_samples > 0 && orders >= cfg.t2t_samples) return;
            if (q.ask_qty <= 0 || q.ask_px < q.bid_px) return;
            if (q.ask_px - q.bid_px > cfg.max_spread_x10000) return;
            if (q.ask_px > cfg.buy_px_x10000) return;
            const int64_t now = q.local_us > 0 ? q.local_us : 1;
            if (cfg.min_order_gap_us > 0 && now - last_us < cfg.min_order_gap_us) return;
            if (live >= cfg.max_live_orders) return;
            uint64_t c0 = 0;
            (void)pitlane::cycles_ordered(c0);
            pitlane::Intent in{};
            in.tag = mint.next(1, 10);
            in.name = q.name;
            in.acct = 1;
            in.px = q.ask_px;
            in.qty = cfg.order_qty > 0 ? cfg.order_qty : 1;
            in.stamp_us = now;
            in.side = static_cast<uint8_t>(pitlane::Side::Buy);
            in.act = static_cast<uint8_t>(pitlane::Act::Enter);
            in.style = static_cast<uint8_t>(pitlane::Style::Immediate);
            if (!insert_order(in)) return;
            uint64_t c1 = 0;
            (void)pitlane::cycles_ordered(c1);
            const int i = orders;
            if (i < 64) {
                tags[i] = in.tag;
                t0[i] = c0;
                uint64_t ns = 0;
                if (c1 > c0) (void)pitlane::scale_ns(scale, c1 - c0, ns);
                local_ns[i] = ns;
            }
            ++orders;
            ++live;
            last_us = now;
        }

        void on_report(const pitlane::Report& r) override {
            if (r.phase != static_cast<uint8_t>(pitlane::Phase::Filled) || r.qty <= 0) return;
            uint64_t c1 = 0;
            (void)pitlane::cycles_ordered(c1);
            for (int i = 0; i < orders && i < 64; ++i) {
                if (tags[i] != r.tag || round_ns[i] != 0) continue;
                uint64_t ns = 0;
                if (c1 > t0[i]) (void)pitlane::scale_ns(scale, c1 - t0[i], ns);
                round_ns[i] = ns;
                ++fills;
                if (live > 0) --live;
                break;
            }
        }
    } hot;
    hot.cfg = cfg;
    hot.scale = scale;

    pitlane::VenueCfg vc{};
    auto* venue = new (std::nothrow) pitlane::PaperVenue(vc);
    if (!venue) return 1;
    pitlane::TradeLane lane{};
    lane.venue = venue;
    lane.report_mask = 1ull << pitlane::kLoopReader;

    pitlane::Center center{};
    center.desk = desk;
    center.md = &md;
    center.strat = &hot;
    center.paper = &lane;
    center.book = venue;
    center.log = &wlog;
    center.heartbeat_us = 1000000;
    if (!center.set_up()) return 1;

    const int want = cfg.t2t_samples > 0 ? cfg.t2t_samples : 8;
    for (int i = 0; i < kTape + want + 4 && hot.fills < want; ++i) center.step(1000 + i);

    uint64_t local[64];
    uint64_t rt[64];
    int nl = 0, nr = 0;
    const int use = hot.fills < 64 ? hot.fills : 64;
    for (int i = 0; i < hot.orders && i < 64; ++i) {
        if (hot.local_ns[i]) local[nl++] = hot.local_ns[i];
        if (hot.round_ns[i]) rt[nr++] = hot.round_ns[i];
    }
    auto sortn = [](uint64_t* a, int n) {
        for (int i = 1; i < n; ++i) {
            uint64_t v = a[i];
            int j = i;
            while (j > 0 && a[j - 1] > v) {
                a[j] = a[j - 1];
                --j;
            }
            a[j] = v;
        }
    };
    auto pct = [&](uint64_t* a, int n, int p) -> uint64_t {
        if (n <= 0) return 0;
        sortn(a, n);
        return a[(n - 1) * p / 100];
    };
    std::printf("\n=== pitlane %s simnow paper center ===\n", pitlane::kVersion);
    std::printf("orders=%d fills=%d beats=%llu\n", hot.orders, hot.fills, static_cast<unsigned long long>(center.beats));
    if (nl)
        std::printf("local_path_ns p50=%llu p99=%llu\n", static_cast<unsigned long long>(pct(local, nl, 50)),
                    static_cast<unsigned long long>(pct(local, nl, 99)));
    if (nr)
        std::printf("roundtrip_ns  p50=%llu p99=%llu\n", static_cast<unsigned long long>(pct(rt, nr, 50)),
                    static_cast<unsigned long long>(pct(rt, nr, 99)));
    std::printf("============================================\n");
    wlog.log(2, pitlane::lit("paper fills="), hot.fills);
    wlog.flush();
    delete venue;
    pitlane::map_release(&map);
    ::unlink(acct.desk_path.c_str());
    return hot.fills > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    // Prefer running from examples/simnow so ./run stays local to the example.
    const char* conf_path = "conf/simnow_7x24.conf";
    if (argc > 1) conf_path = argv[1];

    pitlane::Account acct{};
    pitlane::StratCfg strat{};
    std::string err;
    if (!pitlane::load_account_conf(conf_path, acct, strat, err)) {
        std::fprintf(stderr, "load conf %s: %s\n", conf_path, err.c_str());
        std::fprintf(stderr, "usage: cd examples/simnow && ../../build/pitlane_simnow conf/simnow_7x24.conf\n");
        return 2;
    }
    pitlane::apply_account_env(acct);
    pitlane::resolve_runtime_paths(acct);
    if (strat.paper) return run_paper(acct, strat);
    if (acct.password.empty() || acct.investor_id.empty() || acct.instruments.empty()) {
        std::fprintf(stderr, "need investor_id/password/instruments\n");
        return 2;
    }
    if (!ensure_dir(acct.work_dir.c_str()) || !ensure_dir(acct.flow_md_dir.c_str()) || !ensure_dir(acct.flow_td_dir.c_str())) {
        std::fprintf(stderr, "mkdir work/flow failed under %s\n", acct.work_dir.c_str());
        return 2;
    }

    if (acct.host_cpu >= 0) (void)pitlane::pin_thread(acct.host_cpu);

    ::unlink(acct.desk_path.c_str());
    pitlane::MapOpt opt{};
    opt.hugepage = 0;
    opt.lock = 0;
    opt.prefault = 1;
    pitlane::MapInfo map{};
    if (pitlane::map_create(acct.desk_path.c_str(), pitlane::desk_bytes(), &map, opt) != 0) return 1;
    pitlane::Desk* desk = pitlane::desk_view(&map, 1);
    if (!desk) return 1;

    App app;
    app.cfg = strat;
    app.desk = desk;
    g_app = &app;
    if (!warm_scale(app.scale)) return 1;

    pitlane::WireLog wlog(acct.log_path.c_str(), 3);
    wlog.log(2, pitlane::lit("simnow start investor="), acct.investor_id.c_str(), pitlane::lit(" work="),
             acct.work_dir.c_str());

    pitlane::CtpMdEngine md(acct);
    pitlane::CtpTdClient td(acct);
    md.set_log(&wlog);
    td.set_log(&wlog);
    pitlane::TdEngine td_eng;
    app.td = &td;

    md.bind_desk(desk);
    td_eng.bind_desk(desk);
    td_eng.client = &td;
    td.set_report_sink(&td_eng);
    td.set_scale(&app.scale);
    td.set_accept(on_accept, &app);
    md.set_tick_fn(on_tick_hot, &app);
    md.quote_mask = 0;

    if (!md.set_up()) return 1;
    td.bind_inst(md.inst());
    if (!td.start()) return 1;
    md.pre_run();

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    timespec wait_t0{};
    clock_gettime(CLOCK_MONOTONIC, &wait_t0);
    std::printf("[simnow] pitlane %s investor=%s n_inst=%zu samples=%d work=%s (live IOC)\n", pitlane::kVersion,
                acct.investor_id.c_str(), acct.instruments.size(), strat.t2t_samples, acct.work_dir.c_str());

    // Busy-wait for completion; no sleep on the wait path beyond cpu_relax bursts.
    uint64_t spins = 0;
    while (!app.stop.load(std::memory_order_acquire)) {
        if (td.failed() || md.failed()) {
            wlog.log(0, pitlane::lit("simnow stop: broker login failed"));
            app.stop.store(true, std::memory_order_release);
            break;
        }
        if (!td.ready() && strat.connect_timeout_s > 0) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - wait_t0.tv_sec >= strat.connect_timeout_s) {
                wlog.log(0, pitlane::lit("simnow stop: front silent for "), strat.connect_timeout_s, pitlane::lit("s"));
                app.stop.store(true, std::memory_order_release);
                break;
            }
        }
        ++spins;
        md.loop_task();
        if ((spins & 0xffffull) == 0) {
            // light yield every 64k spins to avoid locking a core forever if quiet
            timespec ts{0, 100000}; // 0.1ms
            nanosleep(&ts, nullptr);
        } else {
            pitlane::cpu_relax();
        }
    }

    const int rc = (td.ready() && !td.failed() && !md.failed()) ? 0 : 1;
    wlog.flush();
    summarize(app);
    md.request_stop();
    md.post_run();
    td.stop();
    wlog.flush();
    pitlane::map_release(&map);
    ::unlink(acct.desk_path.c_str());
    return rc;
}
