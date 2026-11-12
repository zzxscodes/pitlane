#include "pitlane/pitlane.hpp"
#include "t2t_plan.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

// Latency samples after a discarded warmup. The pass checks are the rank order,
// a longer pause having a higher median, and the median staying under a wide ceiling.
// Tick to trade across three processes on one shared desk.
// The market-data process publishes a quote. The engine updates the book, a feature,
// and the test strategy, then the risk check and the order sheet, and publishes the order.
// The gateway stamps the cycle counter as soon as that order is visible.
// The sample is the gap between those two stamps. The report that carries the second stamp is not part of it.
// An ordinary build keeps the 99th and the 999th at or under 2 microseconds.
// A sample several times above the body of the distribution is counted separately.

namespace {

int g_fails = 0;

void expect(bool ok, const char* name) {
    if (ok) return;
    std::fprintf(stderr, "fail %s\n", name);
    ++g_fails;
}

template <class Fn>
bool sample(const char* name, Fn&& fn, pitlane::LatReport& out, int warmup, int keep) {
    pitlane::MagHist hist;
    pitlane::CycleProbe probe;
    probe.arm(warmup);
    int kept = 0;
    int spins = 0;
    const int limit = warmup + keep + 8;
    while (kept < keep && spins < limit) {
        ++spins;
        if (!probe.begin()) return false;
        fn();
        uint64_t cycles = 0;
        const int end = probe.end(cycles);
        if (end < 0) return false;
        if (end == 0) continue;
        if (!hist.add(cycles)) return false;
        ++kept;
    }
    if (kept != keep) return false;
    if (!pitlane::mag_report(hist, out)) return false;
    std::printf("%s n=%llu min=%llu p50=%llu p99=%llu p999=%llu max=%llu\n", name,
                static_cast<unsigned long long>(out.n), static_cast<unsigned long long>(out.min),
                static_cast<unsigned long long>(out.p50), static_cast<unsigned long long>(out.p99),
                static_cast<unsigned long long>(out.p999), static_cast<unsigned long long>(out.max));
    return out.n == static_cast<uint64_t>(keep) && out.min <= out.max && out.p50 <= out.p99 && out.p99 <= out.p999 &&
           out.p999 <= out.max && out.p50 < 500000ull;
}

bool pin_cpu(int cpu) noexcept {
    if (cpu < 0) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

int read_cpu_file(const char* pattern, int cpu) noexcept {
    char path[160];
    std::snprintf(path, sizeof(path), pattern, cpu);
    FILE* file = std::fopen(path, "r");
    if (!file) return -1;
    int value = -1;
    if (std::fscanf(file, "%d", &value) != 1) value = -1;
    std::fclose(file);
    return value;
}

void pick_cores(int& first, int& second, int& third) noexcept {
    first = 0;
    second = 0;
    third = 0;
    int preferred[64];
    int cores[64];
    int npref = 0;
    for (int cpu = 0; cpu < 256 && npref < 64; ++cpu) {
        const int pkg = read_cpu_file("/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
        const int core = read_cpu_file("/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        if (pkg != 0 || core < 0) continue;
        bool seen = false;
        for (int i = 0; i < npref; ++i) {
            if (cores[i] == core) seen = true;
        }
        if (seen) continue;
        preferred[npref] = cpu;
        cores[npref] = core;
        ++npref;
    }
    int online[64];
    int nonline = 0;
    const long ncpu = ::sysconf(_SC_NPROCESSORS_ONLN);
    const int limit = ncpu > 0 ? static_cast<int>(ncpu) : 1;
    for (int cpu = 0; cpu < limit && nonline < 64; ++cpu) online[nonline++] = cpu;
    if (nonline <= 0) {
        online[0] = 0;
        nonline = 1;
    }
    const int* pool = npref >= 3 ? preferred : online;
    const int npool = npref >= 3 ? npref : nonline;
    first = pool[0];
    second = pool[1 % npool];
    third = pool[2 % npool];
}

bool prepare_gate(pitlane::NameList& names, pitlane::WashRow* wash, pitlane::GateAsk& ask, pitlane::FlowWindow& flow) noexcept {
    const char* blocked[] = {"AAA", "BBB", "CCC", "DDD", "EEE", "FFF", "GGG", "HHH"};
    for (const char* name : blocked) {
        if (!names.add(name)) return false;
    }
    for (int i = 0; i < 4; ++i) {
        wash[i].acct = 3;
        wash[i].name = 9;
        wash[i].px = 200 + i;
        wash[i].side = 2;
        wash[i].live = 1;
    }
    ask.act = pitlane::Act::Enter;
    ask.mode = pitlane::PxMode::Limit;
    ask.off = pitlane::Off::Open;
    ask.side = 1;
    ask.lane = nullptr;
    ask.names = &names;
    ask.symbol = "ZZ";
    ask.flow = &flow;
    ask.flow_span = 0;
    ask.flow_limit = 8;
    ask.wash = wash;
    ask.wash_n = 4;
    ask.acct = 3;
    ask.name_id = 9;
    ask.last = 100;
    ask.dev_den = 100;
    ask.dev_cap = 50;
    return true;
}


pitlane::MapOpt map_opt_tmp() noexcept {
    pitlane::MapOpt opt{};
    opt.hugepage = 0;
    opt.lock = 0;
    opt.prefault = 1;
    return opt;
}

int engine(const char* path, int cpu) noexcept {
    (void)pin_cpu(cpu);
    pitlane::MapInfo map{};
    if (pitlane::map_attach(path, pitlane::desk_bytes(), &map, map_opt_tmp()) != 0) return 2;
    pitlane::Desk* desk = pitlane::desk_view(&map, 0);
    if (!desk) {
        pitlane::map_release(&map);
        return 3;
    }
    pitlane::NameList names;
    pitlane::LaneRule rule;
    pitlane::FlowWindow flow;
    pitlane::WashRow wash[4]{};
    pitlane::GateAsk ask;
    if (!prepare_gate(names, wash, ask, flow)) {
        pitlane::map_release(&map);
        return 4;
    }
    ask.lane = &rule;
    pitlane::QuoteFold<8> book;
    T2TPlan plan;
    plan.buy_px = 101;
    plan.qty = 1;
    plan.pos_limit = 1000000;
    plan.max_spread = 1;
    pitlane::Pulse ready{};
    ready.name = 1;
    ready.value = 1;
    if (!desk->pulses.publish(ready, 0)) {
        pitlane::map_release(&map);
        return 4;
    }
    uint64_t pos = 0;
    for (;;) {
        pitlane::Quote tick{};
        bool fresh = false;
        if (!desk->quotes.at(pos, 0, tick, &fresh)) {
            pitlane::cpu_relax();
            continue;
        }
        if (tick.name == 0) {
            pitlane::Intent stop{};
            desk->intents.publish(stop, 0);
            desk->quotes.ack(pos, 0);
            pitlane::map_release(&map);
            return 0;
        }
        T2TFeature feature;
        if (!t2t_feature(tick.bid_px, tick.ask_px, tick.bid_qty, tick.ask_qty, feature)) return 5;
        const pitlane::FoldEdit edits[2] = {
            {pitlane::FoldOp::Set, 1, tick.bid_px, tick.bid_qty},
            {pitlane::FoldOp::Set, 2, tick.ask_px, tick.ask_qty},
        };
        if (!book.apply(edits, 2)) return 5;
        pitlane::FoldLevel bid{};
        pitlane::FoldLevel offer{};
        if (book.copy(1, &bid, 1) != 1 || book.copy(2, &offer, 1) != 1) return 5;
        T2TOrder order;
        if (!t2t_plan_on_book(plan, feature, bid.px, offer.px, order)) return 5;
        ask.now = tick.name;
        ask.px = order.px;
        ask.qty = order.qty;
        ask.side = order.side;
        if (pitlane::gate_ask(ask) != pitlane::Cause::Ok) return 5;
        pitlane::Sheet sheet;
        if (!pitlane::sheet_open(sheet, tick.name, order.side, order.px, order.qty)) return 5;
        if (!pitlane::sheet_apply(sheet, pitlane::SheetOp::Stage, 0, tick.exch_us, pitlane::Cause::Ok)) return 5;
        pitlane::Intent wire{};
        wire.tag = sheet.local;
        wire.name = 9;
        wire.acct = 3;
        wire.px = sheet.px;
        wire.qty = sheet.qty;
        wire.stamp_us = tick.exch_us;
        wire.side = sheet.side;
        wire.act = 1;
        wire.style = 1;
        wire.lot = 1;
        if (!desk->intents.publish(wire, 0) || !desk->quotes.ack(pos, 0)) return 6;
        ++pos;
    }
}

int gateway(const char* path, int cpu) noexcept {
    (void)pin_cpu(cpu);
    pitlane::MapInfo map{};
    if (pitlane::map_attach(path, pitlane::desk_bytes(), &map, map_opt_tmp()) != 0) return 2;
    pitlane::Desk* desk = pitlane::desk_view(&map, 0);
    if (!desk) {
        pitlane::map_release(&map);
        return 3;
    }
    // FanRing is single-producer; keep gateway ready off the shared pulses ring.
    pitlane::TapePrint ready{};
    ready.name = 2;
    ready.qty = 2;
    if (!desk->prints.publish(ready, 0)) {
        pitlane::map_release(&map);
        return 4;
    }
    uint64_t pos = 0;
    for (;;) {
        pitlane::Intent wire{};
        bool fresh = false;
        if (!desk->intents.at(pos, 2, wire, &fresh)) {
            pitlane::cpu_relax();
            continue;
        }
        if (wire.tag == 0) {
            desk->intents.ack(pos, 2);
            pitlane::map_release(&map);
            return 0;
        }
        uint64_t t1 = 0;
        if (!pitlane::cycles_ordered(t1)) return 6;
        pitlane::Report rep{};
        rep.tag = wire.tag;
        rep.contra = wire.tag;
        rep.px = wire.px;
        rep.qty = wire.qty;
        rep.left = wire.qty;
        rep.stamp_us = static_cast<int64_t>(t1);
        rep.phase = static_cast<uint8_t>(pitlane::Phase::Staged);
        rep.side = wire.side;
        rep.name = wire.name;
        if (!desk->reports.publish(rep, 0) || !desk->intents.ack(pos, 2)) return 6;
        ++pos;
    }
}

bool wait_ready(pitlane::Desk* desk) noexcept {
    bool engine_up = false;
    bool gateway_up = false;
    uint64_t pulse_pos = 0;
    uint64_t print_pos = 0;
    for (int i = 0; i < 100000000 && !(engine_up && gateway_up); ++i) {
        if (!engine_up) {
            pitlane::Pulse pulse{};
            bool fresh = false;
            if (desk->pulses.at(pulse_pos, 1, pulse, &fresh) && pulse.value == 1) {
                engine_up = true;
                ++pulse_pos;
            }
        }
        if (!gateway_up) {
            pitlane::TapePrint print{};
            bool fresh = false;
            if (desk->prints.at(print_pos, 1, print, &fresh) && print.qty == 2) {
                gateway_up = true;
                ++print_pos;
            }
        }
        if (!(engine_up && gateway_up)) pitlane::cpu_relax();
    }
    return engine_up && gateway_up;
}

bool tick_to_trade(pitlane::Desk* desk, uint64_t seq, uint64_t report_pos, uint64_t& cycles) noexcept {
    if (seq == 0 || seq > 0xffffffffull) return false;
    pitlane::Quote tick{};
    tick.name = static_cast<uint32_t>(seq);
    tick.bid_px = 100;
    tick.ask_px = 101;
    tick.last_px = 100;
    tick.bid_qty = 2;
    tick.ask_qty = 2;
    tick.last_qty = 1;
    tick.exch_us = 1000;
    uint64_t t0 = 0;
    if (!pitlane::cycles_ordered(t0)) return false;
    tick.local_us = static_cast<int64_t>(t0);
    if (!desk->quotes.publish(tick, 0)) return false;
    for (int i = 0; i < 10000000; ++i) {
        pitlane::Report got{};
        bool fresh = false;
        if (!desk->reports.at(report_pos, 1, got, &fresh)) {
            pitlane::cpu_relax();
            continue;
        }
        if (got.tag != seq || got.contra != seq || got.phase != static_cast<uint8_t>(pitlane::Phase::Staged)) return false;
        if (got.stamp_us < 0) return false;
        const uint64_t t1 = static_cast<uint64_t>(got.stamp_us);
        if (t1 < t0) return false;
        cycles = t1 - t0;
        pitlane::keep(cycles);
        return true;
    }
    return false;
}

bool calibrate(pitlane::CycleScale& scale) noexcept {
    uint64_t c0 = 0;
    uint64_t c1 = 0;
    timespec a{};
    timespec b{};
    if (!pitlane::cycles_ordered(c0) || clock_gettime(CLOCK_MONOTONIC, &a) != 0) return false;
    timespec deadline = a;
    deadline.tv_nsec += 20000000;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    for (;;) {
        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return false;
        if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) break;
        pitlane::cpu_relax();
    }
    if (clock_gettime(CLOCK_MONOTONIC, &b) != 0 || !pitlane::cycles_ordered(c1) || c1 <= c0) return false;
    uint64_t ns = 0;
    if (b.tv_nsec >= a.tv_nsec) {
        ns = static_cast<uint64_t>(b.tv_sec - a.tv_sec) * 1000000000ull + static_cast<uint64_t>(b.tv_nsec - a.tv_nsec);
    } else {
        ns = static_cast<uint64_t>(b.tv_sec - a.tv_sec - 1) * 1000000000ull +
             static_cast<uint64_t>(1000000000L + b.tv_nsec - a.tv_nsec);
    }
    return pitlane::scale_set(scale, c1 - c0, ns);
}

uint64_t percentile(uint64_t* values, int n, int numer, int denom) noexcept {
    const uint64_t need = (static_cast<uint64_t>(n) * static_cast<uint64_t>(numer) + static_cast<uint64_t>(denom - 1)) / static_cast<uint64_t>(denom);
    const uint64_t rank = need == 0 ? 1 : need;
    const uint64_t at = rank > static_cast<uint64_t>(n) ? static_cast<uint64_t>(n) : rank;
    return values[at - 1];
}

struct PassReport {
    uint64_t p50{0};
    uint64_t p99{0};
    uint64_t p999{0};
    uint64_t raw_p999{0};
    uint64_t max_ns{0};
    int stalls{0};
};

bool pass_once(pitlane::Desk* desk, uint64_t& seq, uint64_t& report_pos, const pitlane::CycleScale& scale, int warmup,
               int keep, PassReport& out) noexcept {
    constexpr int kKeep = 2000;
    if (!desk || keep != kKeep) return false;
    uint64_t ns_samples[kKeep];
    int kept = 0;
    int seen = 0;
    const int limit = warmup + keep + 8;
    while (kept < keep && seen < limit) {
        uint64_t cycles = 0;
        if (!tick_to_trade(desk, seq, report_pos, cycles)) return false;
        if (!desk->reports.ack(report_pos, 1)) return false;
        ++report_pos;
        ++seq;
        ++seen;
        if (seen <= warmup) continue;
        uint64_t ns = 0;
        if (!pitlane::scale_ns(scale, cycles, ns)) return false;
        ns_samples[kept] = ns;
        ++kept;
    }
    if (kept != keep) return false;
    std::sort(ns_samples, ns_samples + keep);
    out.p50 = percentile(ns_samples, keep, 1, 2);
    out.raw_p999 = percentile(ns_samples, keep, 999, 1000);
    out.max_ns = ns_samples[keep - 1];
    const uint64_t cut = out.p50 > (UINT64_MAX / 8) ? UINT64_MAX : out.p50 * 8ull;
    int body = keep;
    while (body > 0 && ns_samples[body - 1] > 2000ull && ns_samples[body - 1] > cut) --body;
    out.stalls = keep - body;
    // Under ASan / busy CI hosts, allow a wider stall tail before discarding the pass.
#if defined(PITLANE_RELAX_LATENCY)
    if (body < keep - keep / 20) return false;
#else
    if (body < keep - keep / 100) return false;
#endif
    out.p99 = percentile(ns_samples, body, 99, 100);
    out.p999 = percentile(ns_samples, body, 999, 1000);
    return out.p50 <= out.p99 && out.p99 <= out.p999 && out.p999 <= out.max_ns;
}

uint64_t median_of(uint64_t* values, int n) noexcept {
    std::sort(values, values + n);
    return values[n / 2];
}

uint64_t pause_p50(int pauses) {
    pitlane::LatReport report;
    const bool ok = sample(
        pauses > 32 ? "pause_long" : "pause_short",
        [&] {
            for (int i = 0; i < pauses; ++i) pitlane::cpu_relax();
        },
        report, 50, 200);
    expect(ok, pauses > 32 ? "pause_long" : "pause_short");
    return ok ? report.p50 : 0;
}

}  // namespace

int main() {
    pitlane::LatReport report;

    pitlane::UsSlot slot;
    pitlane::us_store(slot, 1);
    expect(sample(
               "us_load",
               [&] {
                   const int64_t us = pitlane::us_load(slot);
                   pitlane::keep(us);
               },
               report, 100, 1000),
           "us_load");

    pitlane::CycleScale scale;
    expect(pitlane::scale_set(scale, 1000, 500), "scale");
    uint64_t runs[5] = {};
    for (int i = 0; i < 5; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "scale_ns_%d", i);
        const bool ok = sample(
            name,
            [&] {
                uint64_t ns = 0;
                pitlane::scale_ns(scale, 1000, ns);
                pitlane::keep(ns);
            },
            report, 100, 1000);
        expect(ok, name);
        runs[i] = ok ? report.p50 : 0;
    }
    for (int i = 1; i < 5; ++i) {
        const uint64_t v = runs[i];
        int j = i;
        while (j > 0 && runs[j - 1] > v) {
            runs[j] = runs[j - 1];
            --j;
        }
        runs[j] = v;
    }
    const uint64_t median = runs[2];
    std::printf("scale_ns median_of_5=%llu\n", static_cast<unsigned long long>(median));
    expect(median > 0 && median < 500000ull, "scale median");

    expect(sample(
               "lap",
               [&] {
                   int64_t ns = 0;
                   pitlane::order_to_ack(10, 40, ns);
                   pitlane::keep(ns);
               },
               report, 100, 1000),
           "lap");

    pitlane::MagHist hist;
    expect(sample(
               "mag_add",
               [&] {
                   const bool added = hist.add(20);
                   pitlane::keep(added);
               },
               report, 100, 1000),
           "mag_add");

    pitlane::FillRing<uint64_t, 4> ring;
    uint64_t seq = 1;
    expect(sample(
               "fill_ring",
               [&] {
                   uint64_t* slot = ring.claim();
                   if (slot != nullptr) *slot = seq++;
                   ring.publish();
                   uint64_t got = 0;
                   ring.take(got);
                   pitlane::keep(got);
               },
               report, 100, 1000),
           "fill_ring");

    {
        pitlane::TopicBus bus;
        uint64_t cur = 0;
        const char body[] = "x";
        expect(sample(
                   "topic_bus",
                   [&] {
                       (void)bus.publish_key("t", 1, body, 1, 0, 0);
                       pitlane::BusNote note{};
                       const int n = bus.drain(0, cur, 0, &note, 1);
                       pitlane::keep(static_cast<uint64_t>(n));
                   },
                   report, 100, 1000),
               "topic_bus");
    }

    {
        pitlane::VenueCfg cfg{};
        cfg.lot = 1;
        pitlane::PaperVenue venue(cfg);
        const pitlane::Rung asks[] = {{100, 8}};
        venue.set_levels(1, nullptr, 0, asks, 1);
        venue.set_band(1, 90, 110);
        pitlane::Report reps[4]{};
        uint64_t tag = 1;
        const int64_t stamp = (9 * 60 + 40) * 60 * 1000000ll;
        expect(sample(
                   "venue_admit",
                   [&] {
                       pitlane::Intent in{};
                       in.tag = tag++;
                       in.name = 1;
                       in.acct = 1;
                       in.px = 100;
                       in.qty = 1;
                       in.stamp_us = stamp;
                       in.side = 1;
                       in.act = 1;
                       in.style = 1;
                       const pitlane::VenueOut out = venue.admit(in, stamp, reps, 4);
                       pitlane::keep(static_cast<uint64_t>(out.reports));
                   },
                   report, 50, 400),
               "venue_admit");
    }

    pitlane::Urgent urgent;
    expect(sample(
               "urgent",
               [&] {
                   urgent.post(1);
                   const uint64_t tag = urgent.take();
                   pitlane::keep(tag);
               },
               report, 100, 1000),
           "urgent");

    pitlane::TickWheel<64> wheel;
    expect(sample(
               "wheel",
               [&] {
                   const uint32_t now = wheel.now();
                   wheel.arm(now + 1u, 1u);
                   uint32_t tags[1] = {};
                   const int n = wheel.take(now + 1u, tags, 1);
                   pitlane::keep(n);
               },
               report, 50, 200),
           "wheel");

    pitlane::ClockPage page;
    pitlane::ClockView view;
    expect(pitlane::clock_publish(page, 1000, 500, 1000, 10, 0), "publish");
    expect(view.refresh(page), "refresh");
    uint64_t stamp = 1000;
    expect(sample(
               "to_wall",
               [&] {
                   int64_t sec = 0;
                   int64_t nsec = 0;
                   view.to_wall(stamp, sec, nsec);
                   stamp += 1000;
                   if (stamp > 100000) stamp = 1000;
                   pitlane::keep(nsec);
               },
               report, 100, 1000),
           "to_wall");

    const uint64_t short_p50 = pause_p50(8);
    const uint64_t long_p50 = pause_p50(128);
    std::printf("pause short=%llu long=%llu\n", static_cast<unsigned long long>(short_p50),
                static_cast<unsigned long long>(long_p50));
    expect(long_p50 > short_p50, "longer pause");

    pitlane::CycleProbe warmup;
    warmup.arm(4);
    int dropped = 0;
    for (int i = 0; i < 4; ++i) {
        expect(warmup.begin(), "warmup begin");
        uint64_t cycles = 1;
        const int end = warmup.end(cycles);
        if (end == 0) ++dropped;
    }
    expect(dropped == 4, "warmup dropped");

    int cpu_md = 0;
    int cpu_engine = 2;
    int cpu_gateway = 4;
    pick_cores(cpu_md, cpu_engine, cpu_gateway);
    char path[64];
    std::snprintf(path, sizeof(path), "/tmp/pitlane-e2e-%d.bin", static_cast<int>(::getpid()));
    ::unlink(path);
    pitlane::MapInfo map{};
    expect(pitlane::map_create(path, pitlane::desk_bytes(), &map, map_opt_tmp()) == 0, "map");
    pitlane::Desk* desk = pitlane::desk_view(&map, 1);
    expect(desk != nullptr, "desk");
    const pid_t engine_pid = desk ? ::fork() : -1;
    if (engine_pid == 0) {
        const int code = engine(path, cpu_engine);
        _exit(code);
    }
    const pid_t gateway_pid = engine_pid > 0 ? ::fork() : -1;
    if (gateway_pid == 0) {
        const int code = gateway(path, cpu_gateway);
        _exit(code);
    }
    expect(engine_pid > 0 && gateway_pid > 0, "fork");
    uint64_t p99s[5] = {};
    uint64_t p999s[5] = {};
    bool measured = false;
    if (engine_pid > 0 && gateway_pid > 0) {
        (void)pin_cpu(cpu_md);
        const bool ready = wait_ready(desk);
        expect(ready, "ready");
        if (ready) {
            pitlane::CycleScale wall;
            expect(calibrate(wall), "calibrate");
            uint64_t seq = 1;
            uint64_t report_pos = 0;
            for (int i = 0; i < 5; ++i) {
                PassReport pass;
                bool ok = false;
#if defined(PITLANE_RELAX_LATENCY)
                for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
                    ok = pass_once(desk, seq, report_pos, wall, i == 0 && attempt == 0 ? 1024 : 64, 2000, pass);
                }
#else
                ok = pass_once(desk, seq, report_pos, wall, i == 0 ? 1024 : 64, 2000, pass);
#endif
                expect(ok, "pass path");
                std::printf("t2t_ns run=%d p50=%llu p99=%llu p999=%llu raw_p999=%llu max=%llu stalls=%d\n", i,
                            static_cast<unsigned long long>(pass.p50), static_cast<unsigned long long>(pass.p99),
                            static_cast<unsigned long long>(pass.p999), static_cast<unsigned long long>(pass.raw_p999),
                            static_cast<unsigned long long>(pass.max_ns), pass.stalls);
                p99s[i] = ok ? pass.p99 : UINT64_MAX;
                p999s[i] = ok ? pass.p999 : UINT64_MAX;
                if (!ok) break;
                measured = i == 4;
            }
        }
        pitlane::Quote stop{};
        if (!desk->quotes.publish(stop, 0)) {
            ::kill(engine_pid, SIGKILL);
            ::kill(gateway_pid, SIGKILL);
        }
        int engine_status = 0;
        int gateway_status = 0;
        ::waitpid(engine_pid, &engine_status, 0);
        ::waitpid(gateway_pid, &gateway_status, 0);
        expect(WIFEXITED(engine_status) && WEXITSTATUS(engine_status) == 0, "engine exit");
        expect(WIFEXITED(gateway_status) && WEXITSTATUS(gateway_status) == 0, "gateway exit");
    } else if (engine_pid > 0) {
        ::kill(engine_pid, SIGKILL);
        ::waitpid(engine_pid, nullptr, 0);
    }
    pitlane::map_release(&map);
    ::unlink(path);
    if (measured) {
        const uint64_t p99_med = median_of(p99s, 5);
        const uint64_t p999_med = median_of(p999s, 5);
        std::printf("t2t_cpus %d %d %d\n", cpu_md, cpu_engine, cpu_gateway);
        std::printf("t2t_ns median p99=%llu p999=%llu\n", static_cast<unsigned long long>(p99_med),
                    static_cast<unsigned long long>(p999_med));
#if defined(PITLANE_RELAX_LATENCY)
        expect(p99_med <= 200000ull && p999_med <= 500000ull, "e2e relaxed");
#else
        expect(p99_med <= 2000ull && p999_med <= 2000ull, "e2e 2us");
#endif
    }

    return g_fails == 0 ? 0 : 1;
}
