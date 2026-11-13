#pragma once

// Component times and the paper-center path the simnow example runs without a broker.
// One call is timed with the cycle counter. A hop is the gap between two of those calls.
// The same probes, histograms, and lap helpers are checked here, then used to time the library.

#include "pitlane/pitlane.hpp"

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace pitlane_bench {

inline int g_bench_fails = 0;

inline void expect(bool ok, const char* name) {
    if (ok) return;
    std::fprintf(stderr, "fail %s\n", name);
    ++g_bench_fails;
}

inline bool ordered(const pitlane::LatReport& r) {
    return r.n > 0 && r.min <= r.max && r.p50 <= r.p99 && r.p99 <= r.p999 && r.p999 <= r.max && r.p50 < 500000ull;
}

inline void print_report(const char* name, const pitlane::LatReport& r) {
    std::printf("%s n=%llu min=%llu p50=%llu p99=%llu p999=%llu max=%llu\n", name,
                static_cast<unsigned long long>(r.n), static_cast<unsigned long long>(r.min),
                static_cast<unsigned long long>(r.p50), static_cast<unsigned long long>(r.p99),
                static_cast<unsigned long long>(r.p999), static_cast<unsigned long long>(r.max));
}

template <class Fn>
bool time_call(const char* name, Fn&& fn, int warmup, int keep) {
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
    pitlane::LatReport report{};
    if (kept != keep || !pitlane::mag_report(hist, report)) return false;
    print_report(name, report);
    return ordered(report);
}

inline bool add_gap(pitlane::MagHist& hist, uint64_t begin, uint64_t end) {
    uint64_t gap = 0;
    if (!pitlane::cycle_gap(begin, end, gap)) return false;
    return hist.add(gap);
}

inline bool report_hist(const char* name, const pitlane::MagHist& hist) {
    pitlane::LatReport report{};
    if (!pitlane::mag_report(hist, report) || !ordered(report)) return false;
    print_report(name, report);
    return true;
}

inline pitlane::MapOpt map_opt_tmp() noexcept {
    pitlane::MapOpt opt{};
    opt.hugepage = 0;
    opt.lock = 0;
    opt.prefault = 1;
    return opt;
}

inline void check_helpers() {
    pitlane::CycleScale scale{};
    expect(pitlane::scale_set(scale, 1000, 500), "scale set");
    uint64_t ns = 0;
    expect(pitlane::scale_ns(scale, 1000, ns) && ns == 500, "scale 500ns");
    expect(pitlane::scale_ns(scale, 0, ns) && ns == 0, "scale zero");

    int64_t lap = 0;
    expect(pitlane::order_to_ack(10, 40, lap) && lap == 30, "order to ack");
    expect(pitlane::tick_to_trade(10, 40, lap) && lap == 30, "tick to trade");
    expect(!pitlane::tick_to_trade(40, 10, lap), "tick inverted");
    expect(pitlane::tick_to_cancel(10, 25, lap) && lap == 15, "tick to cancel");

    pitlane::LatTrace<4> trace{};
    expect(pitlane::lat_mark(trace, 0, 100, true), "mark 0");
    expect(pitlane::lat_mark(trace, 3, 180, true), "mark 3");
    int64_t span = 0;
    expect(pitlane::lat_span(trace, 0, 3, span) && span == 80, "lat span");
    expect(!pitlane::lat_span(trace, 0, 1, span), "lat missing");

    pitlane::CycleProbe warmup;
    warmup.arm(4);
    int dropped = 0;
    for (int i = 0; i < 4; ++i) {
        expect(warmup.begin(), "warmup begin");
        uint64_t cycles = 0;
        if (warmup.end(cycles) == 0) ++dropped;
    }
    expect(dropped == 4, "warmup dropped");

    int notes = 0;
    const int paced = pitlane::note_paced([&](uint64_t) { ++notes; return true; }, 100, 30);
    expect(paced == 3 && notes == 3, "note paced");
    notes = 0;
    const int stalled = pitlane::note_paced([&](uint64_t) { ++notes; return true; }, 80, 1);
    expect(stalled == -1 && notes == 64, "note cap");

    pitlane::MagHist hist;
    expect(hist.add(10) && hist.add(20) && hist.add(30), "hist add");
    pitlane::LatReport report{};
    expect(pitlane::mag_report(hist, report) && report.n == 3 && report.min == 10 && report.max == 30, "hist report");
    expect(report.p50 >= report.min && report.p50 <= report.max, "hist median");
}

inline void time_components() {
    expect(time_call(
               "probe_floor",
               [] {
                   pitlane::compiler_fence();
               },
               100, 400),
           "probe_floor");

    pitlane::CycleScale scale{};
    expect(pitlane::scale_set(scale, 1000, 500), "component scale");
    expect(time_call(
               "scale_ns",
               [&] {
                   uint64_t ns = 0;
                   pitlane::scale_ns(scale, 1000, ns);
                   pitlane::keep(ns);
               },
               100, 400),
           "scale_ns");

    pitlane::MagHist sink;
    expect(time_call(
               "mag_add",
               [&] {
                   const bool added = sink.add(20);
                   pitlane::keep(added);
               },
               50, 200),
           "mag_add");

    expect(time_call(
               "lap_ns",
               [] {
                   int64_t ns = 0;
                   pitlane::tick_to_trade(10, 40, ns);
                   pitlane::keep(ns);
               },
               100, 400),
           "lap_ns");

    auto* ring = new pitlane::FanRing<pitlane::Quote, 128>();
    uint64_t pos = 0;
    expect(time_call(
               "ring_round",
               [&] {
                   pitlane::Quote q{};
                   q.name = 1;
                   q.bid_px = 100;
                   q.ask_px = 102;
                   const bool published = ring->publish(q, 0);
                   pitlane::Quote got{};
                   const int st = ring->poll(pos, 0, got, false);
                   if (st > 0) ring->ack(pos, 0);
                   ++pos;
                   pitlane::keep(published);
                   pitlane::keep(got.bid_px);
               },
               40, 200),
           "ring_round");
    delete ring;

    expect(time_call(
               "copy_quote",
               [] {
                   pitlane::Quote src{};
                   src.bid_px = 100;
                   src.ask_px = 102;
                   pitlane::Quote dst{};
                   pitlane::copy_bytes(&dst, &src, sizeof src);
                   pitlane::keep(dst.ask_px);
               },
               100, 400),
           "copy_quote");

    expect(time_call(
               "precheck",
               [] {
                   pitlane::Intent in{};
                   in.tag = 1;
                   in.qty = 1;
                   in.stamp_us = 1000;
                   in.side = static_cast<uint8_t>(pitlane::Side::Buy);
                   in.act = static_cast<uint8_t>(pitlane::Act::Enter);
                   const pitlane::Why why = pitlane::order_precheck(in);
                   pitlane::keep(static_cast<uint16_t>(why));
               },
               100, 400),
           "precheck");

    pitlane::NameList names;
    const char* blocked[] = {"AAA", "BBB", "CCC", "DDD", "EEE", "FFF", "GGG", "HHH"};
    for (const char* name : blocked) expect(names.add(name), "block name");
    pitlane::WashRow wash[4]{};
    for (int i = 0; i < 4; ++i) {
        wash[i].acct = 3;
        wash[i].name = 9;
        wash[i].px = 200 + i;
        wash[i].side = 2;
        wash[i].live = 1;
    }
    pitlane::GateAsk ask{};
    ask.act = pitlane::Act::Enter;
    ask.mode = pitlane::PxMode::Limit;
    ask.off = pitlane::Off::Open;
    ask.px = 100;
    ask.qty = 1;
    ask.side = static_cast<uint8_t>(pitlane::Side::Buy);
    ask.names = &names;
    ask.symbol = "ZZ";
    ask.wash = wash;
    ask.wash_n = 4;
    ask.acct = 3;
    ask.name_id = 9;
    ask.last = 100;
    ask.dev_den = 100;
    ask.dev_cap = 50;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::Ok, "gate once");
    expect(time_call(
               "gate_at",
               [&] {
                   const pitlane::Cause cause = pitlane::gate_ask(ask);
                   pitlane::keep(static_cast<uint16_t>(cause));
               },
               50, 200),
           "gate_at");
    ask.px = 110;
    expect(pitlane::gate_ask(ask) == pitlane::Cause::Ok, "gate away once");
    expect(time_call(
               "gate_away",
               [&] {
                   const pitlane::Cause cause = pitlane::gate_ask(ask);
                   pitlane::keep(static_cast<uint16_t>(cause));
               },
               50, 200),
           "gate_away");

    pitlane::VenueCfg cfg{};
    cfg.lot = 1;
    auto* venue = new pitlane::PaperVenue(cfg);
    venue->set_top(1, 1000000, 8, 1000200, 8, 1000100);
    uint64_t tag = 1;
    const int64_t stamp = 1000;
    expect(time_call(
               "book_top",
               [&] {
                   venue->set_top(1, 1000000, 8, 1000200, 8, 1000100);
                   pitlane::keep(tag);
               },
               50, 200),
           "book_top");
    expect(time_call(
               "venue_admit",
               [&] {
                   pitlane::Intent in{};
                   in.tag = tag++;
                   in.name = 1;
                   in.acct = 1;
                   in.px = 1000200;
                   in.qty = 1;
                   in.stamp_us = stamp;
                   in.side = static_cast<uint8_t>(pitlane::Side::Buy);
                   in.act = static_cast<uint8_t>(pitlane::Act::Enter);
                   in.style = static_cast<uint8_t>(pitlane::Style::Immediate);
                   pitlane::Report reps[4]{};
                   const pitlane::VenueOut out = venue->admit(in, stamp, reps, 4);
                   pitlane::keep(static_cast<uint64_t>(out.reports));
               },
               40, 200),
           "venue_admit");
    delete venue;

    char path[64];
    std::snprintf(path, sizeof(path), "/tmp/pitlane-wire-%d.log", static_cast<int>(::getpid()));
    ::unlink(path);
    const int saved_err = ::dup(2);
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        ::dup2(devnull, 2);
        ::close(devnull);
    }
    bool wire_ok = false;
    {
        pitlane::WireLog wlog(path, 3, pitlane::WireLog::kCaller);
        wire_ok = time_call(
            "wire_log",
            [&] {
                const bool wrote = wlog.log(2, pitlane::lit("bench"));
                pitlane::keep(wrote);
            },
            20, 100);
        wlog.flush();
    }
    if (saved_err >= 0) {
        ::dup2(saved_err, 2);
        ::close(saved_err);
    }
    expect(wire_ok, "wire_log");
    ::unlink(path);

    const uint64_t short_p50 = [&] {
        pitlane::LatReport report{};
        pitlane::MagHist hist;
        pitlane::CycleProbe probe;
        probe.arm(30);
        int kept = 0;
        while (kept < 100) {
            if (!probe.begin()) break;
            for (int i = 0; i < 4; ++i) pitlane::cpu_relax();
            uint64_t cycles = 0;
            const int end = probe.end(cycles);
            if (end < 0) break;
            if (end == 0) continue;
            hist.add(cycles);
            ++kept;
        }
        if (!pitlane::mag_report(hist, report)) return uint64_t{0};
        print_report("pause_short", report);
        return report.p50;
    }();
    const uint64_t long_p50 = [&] {
        pitlane::LatReport report{};
        pitlane::MagHist hist;
        pitlane::CycleProbe probe;
        probe.arm(30);
        int kept = 0;
        while (kept < 100) {
            if (!probe.begin()) break;
            for (int i = 0; i < 128; ++i) pitlane::cpu_relax();
            uint64_t cycles = 0;
            const int end = probe.end(cycles);
            if (end < 0) break;
            if (end == 0) continue;
            hist.add(cycles);
            ++kept;
        }
        if (!pitlane::mag_report(hist, report)) return uint64_t{0};
        print_report("pause_long", report);
        return report.p50;
    }();
    expect(long_p50 > short_p50, "pause separates");
}

inline void time_paper() {
    char path[64];
    std::snprintf(path, sizeof(path), "/tmp/pitlane-paper-%d.bin", static_cast<int>(::getpid()));
    ::unlink(path);
    pitlane::MapInfo map{};
    expect(pitlane::map_create(path, pitlane::desk_bytes(), &map, map_opt_tmp()) == 0, "paper map");
    pitlane::Desk* desk = pitlane::desk_view(&map, 1);
    expect(desk != nullptr, "paper desk");
    if (!desk) {
        pitlane::map_release(&map);
        ::unlink(path);
        return;
    }

    pitlane::Quote tape[4]{};
    for (int i = 0; i < 4; ++i) {
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
    md.n = 4;
    md.burst = 1;
    md.quote_mask = (1ull << pitlane::kBookReader) | (1ull << pitlane::kLoopReader);
    md.bind_desk(desk);

    struct Hot : pitlane::StratEngine {
        pitlane::IdMint mint{1, 1};
        int orders{0};
        void on_quote(const pitlane::Quote& q) override {
            pitlane::Intent in{};
            in.tag = mint.next(1, 10);
            in.name = q.name;
            in.acct = 1;
            in.px = q.ask_px;
            in.qty = 1;
            in.stamp_us = q.local_us > 0 ? q.local_us : 1;
            in.side = static_cast<uint8_t>(pitlane::Side::Buy);
            in.act = static_cast<uint8_t>(pitlane::Act::Enter);
            in.style = static_cast<uint8_t>(pitlane::Style::Immediate);
            if (insert_order(in)) ++orders;
        }
    } hot;
    hot.bind_desk(desk);

    pitlane::VenueCfg cfg{};
    cfg.lot = 1;
    auto* venue = new pitlane::PaperVenue(cfg);
    venue->set_top(1, 1000000, 8, 1000200, 8, 1000100);
    pitlane::TradeLane lane{};
    lane.venue = venue;
    lane.report_mask = 1ull << pitlane::kLoopReader;

    expect(md.set_up() && hot.set_up(), "paper setup");

    pitlane::MagHist hop_md;
    pitlane::MagHist hop_book;
    pitlane::MagHist hop_strat;
    pitlane::MagHist hop_match;
    pitlane::MagHist hop_tick;
    constexpr int kWarm = 40;
    constexpr int kKeep = 200;
    int kept = 0;
    int steps = 0;
    uint64_t quote_cur = 0;
    const int64_t now_us = 1000000;
    while (kept < kKeep && steps < kWarm + kKeep + 8) {
        if (md.done()) md.at = 0;
        uint64_t t0 = 0;
        uint64_t t1 = 0;
        uint64_t t2 = 0;
        uint64_t t3 = 0;
        uint64_t t4 = 0;
        if (!pitlane::cycles_ordered(t0)) break;
        md.loop_task();
        if (!pitlane::cycles_ordered(t1)) break;
        pitlane::Quote q{};
        bool fresh = false;
        while (desk->quotes.at(quote_cur, pitlane::kBookReader, q, &fresh)) {
            if (!fresh) break;
            venue->set_top(q.name, q.bid_px, q.bid_qty, q.ask_px, q.ask_qty, q.last_px);
            desk->quotes.ack(quote_cur, pitlane::kBookReader);
            ++quote_cur;
        }
        if (!pitlane::cycles_ordered(t2)) break;
        hot.loop_task();
        if (!pitlane::cycles_ordered(t3)) break;
        (void)pitlane::lane_beat(lane, *desk, now_us);
        if (!pitlane::cycles_ordered(t4)) break;
        ++steps;
        if (steps <= kWarm) continue;
        if (!add_gap(hop_md, t0, t1) || !add_gap(hop_book, t1, t2) || !add_gap(hop_strat, t2, t3) ||
            !add_gap(hop_match, t3, t4) || !add_gap(hop_tick, t0, t4)) {
            break;
        }
        ++kept;
    }
    expect(kept == kKeep, "paper samples");
    expect(hot.orders >= kKeep / 2, "paper orders");
    expect(report_hist("hop_md", hop_md), "hop_md");
    expect(report_hist("hop_book", hop_book), "hop_book");
    expect(report_hist("hop_strat", hop_strat), "hop_strat");
    expect(report_hist("hop_match", hop_match), "hop_match");
    expect(report_hist("hop_tick", hop_tick), "hop_tick");

    delete venue;
    pitlane::map_release(&map);
    ::unlink(path);
}

inline int run() {
    g_bench_fails = 0;
    check_helpers();
    time_components();
    time_paper();
    return g_bench_fails;
}

}  // namespace pitlane_bench
