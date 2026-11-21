#include "pitlane/pitlane.hpp"

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

void check_cycles() {
    uint64_t a = 0;
    uint64_t b = 0;
    expect(pitlane::cycles_read(a) && pitlane::cycles_ordered(b) && b >= a, "cycle reads");
    uint64_t gap = 0;
    expect(pitlane::cycle_gap(a, b, gap) && !pitlane::cycle_gap(b, a, gap), "gap");

    pitlane::UsSlot slot;
    pitlane::us_store(slot, 42);
    expect(pitlane::us_load(slot) == 42, "cached microseconds");

    pitlane::CycleScale scale;
    expect(pitlane::scale_set(scale, 1000, 500), "scale");
    uint64_t ns = 0;
    expect(pitlane::scale_ns(scale, 1000, ns) && ns == 500, "one microsecond pair");
    expect(pitlane::scale_ns(scale, 0, ns) && ns == 0, "zero cycles");
    expect(pitlane::scale_span(scale, 10, 2010, ns) && ns == 1000, "span");
    expect(!pitlane::scale_span(scale, 5, 4, ns), "backward span");

    pitlane::CycleProbe probe;
    probe.arm(1);
    uint64_t cycles = 0;
    expect(probe.begin(), "begin");
    pitlane::keep(cycles);
    expect(probe.end(cycles) == 0, "warmup");
    expect(probe.begin(), "begin again");
    expect(probe.end(cycles) == 1 && cycles > 0, "recorded");

    pitlane::DutyPace pace;
    expect(pace.open(10, 30, 1000), "pace");
    expect(pace.step(0, -1, 0, false) == pitlane::Duty::Fast, "first fast");
    expect(pace.step(5, -1, 0, false) == pitlane::Duty::Skip, "inside fast gap");
    expect(pace.step(10, -1, 0, false) == pitlane::Duty::Fast, "next fast");
    expect(pace.step(1000, 1, 0, true) == pitlane::Duty::Timeout, "timeout");
    expect(pace.step(100, -1, 0, true) == pitlane::Duty::Slow, "slow lap");

    uint64_t due = 0;
    expect(pitlane::deadline_at(10, 5, due) && due == 15, "deadline");
    expect(pitlane::before_deadline(14, 15) && !pitlane::before_deadline(15, 15), "still waiting");
}

void check_hist_and_laps() {
    pitlane::MagHist hist;
    expect(hist.add(0) && hist.add(20) && hist.add(20), "hist");
    uint64_t edge = 1;
    expect(hist.pct(0, 100, edge) && edge == 0, "p0");
    expect(hist.pct(100, 100, edge) && edge == 20, "p100");
    expect(hist.pct(1, 2, edge) && edge == 20, "p50 bucket");
    pitlane::LatReport report;
    expect(pitlane::mag_report(hist, report) && report.n == 3 && report.min == 0 && report.max == 20, "report");
    expect(report.p50 == 20 && report.p99 == 20 && report.p999 == 20, "tails");

    uint64_t got[4]{};
    int n = 0;
    const int paced = pitlane::note_paced(
        [&](uint64_t v) {
            if (n < 4) got[n] = v;
            ++n;
            return true;
        },
        35, 10);
    expect(paced == 3 && got[0] == 35 && got[1] == 25 && got[2] == 15, "paced notes");

    int64_t lap = 0;
    expect(pitlane::order_to_ack(10, 25, lap) && lap == 15, "ack");
    expect(pitlane::tick_to_trade(10, 40, lap) && lap == 30, "trade");
    expect(pitlane::tick_to_cancel(10, 18, lap) && lap == 8, "cancel");
    expect(!pitlane::tick_to_trade(-1, 5, lap), "unset tick");
    expect(pitlane::hop_ns(3, 8, lap) && lap == 5, "hop");
}

void check_clock() {
    pitlane::ClockPage page;
    pitlane::ClockView view;
    expect(!view.refresh(page), "unpublished");
    expect(pitlane::clock_publish(page, 1000, 500, 1000, 10, 0), "publish");
    expect(view.refresh(page), "refresh");
    int64_t sec = 0;
    int64_t nsec = 0;
    expect(view.to_wall(3000, sec, nsec) && sec == 10 && nsec == 1000, "forward");
    expect(view.to_wall(3000, sec, nsec) && nsec == 1000, "cached");
    expect(view.to_wall(0, sec, nsec) && sec == 9 && nsec == 1000000000ll - 500, "behind");
}

void check_wheel() {
    pitlane::TickWheel<8> wheel;
    expect(!wheel.arm(1, 0), "tag");
    expect(wheel.arm(5, 3), "arm");
    expect(wheel.next_deadline() == 5, "next");
    expect(wheel.cancel(3), "cancel");
    uint32_t tags[8]{};
    expect(wheel.take(5, tags, 8) == 0, "cancelled");

    pitlane::TickWheel<8> due;
    expect(due.arm(1, 7) && due.arm(3, 8), "two");
    expect(due.take(3, tags, 8) == 2 && tags[0] == 7 && tags[1] == 8, "fired");
    expect(due.post(2, 9), "post");
    expect(due.take(4, tags, 8) == 1 && tags[0] == 9, "posted fires on the next tick");

    pitlane::TickWheel<8> far;
    expect(far.arm(64, 4), "far");
    expect(far.take(64, tags, 8) == 1 && tags[0] == 4, "cascade");

    pitlane::TickWheel<8> full;
    bool all = true;
    for (uint32_t i = 1; i <= 8; ++i) all = all && full.arm(i, i);
    expect(all && !full.arm(9, 9), "full wheel");
}

static void check_span() {
    pitlane::SpanMark<4> span;
    expect(span.mark(0, 10) && span.mark(2, 25), "mark");
    int64_t dt = 0;
    expect(span.between(0, 2, dt) && dt == 15, "between");
    expect(!span.between(0, 1, dt), "unset node");
}

static void check_tape() {
    pitlane::Urgent urgent;
    expect(urgent.post(5) && !urgent.post(6) && urgent.take() == 5 && urgent.take() == 0, "urgent");

    pitlane::TraceGate gate;
    gate.arm(9, false);
    pitlane::CoreTape<16> tape;
    expect(!tape.note(gate, 11, 2, 3, 4), "gate off");
    gate.arm(9, true);
    expect(tape.note(gate, 11, 2, 3, 4) && gate.event() == 9, "gate on");
    pitlane::CoreTape<16>::Rec rec;
    expect(tape.at(0, rec) && rec.tsc == 11 && rec.a == 3 && rec.b == 4, "tape");

    pitlane::SegTape<2, 4> seg;
    int kept = 0;
    for (int i = 0; i < 7; ++i) kept += seg.push(i, i + 1, static_cast<uint32_t>(i)) ? 1 : 0;
    expect(kept == 7, "segments accepted");
    expect(!seg.push(7, 8, 7) && seg.overrun() == 1, "segment held");
    uint32_t idx = 99;
    expect(seg.take(idx) && idx == 0, "sealed");
    expect(seg.segment(idx)[0].id == 0 && seg.segment(idx)[3].id == 3, "segment body");
    seg.give();

    pitlane::SpikeRing<8> spikes;
    expect(!spikes.signal(1, 5, 1, 0, 10), "under limit");
    expect(spikes.signal(1, 20, 2, 1, 10), "spike");
    pitlane::SpikeRing<8>::Note note;
    expect(spikes.pop(note) && note.seq == 2 && note.cpu == 1 && spikes.dropped() == 0, "spike note");
}

static void check_simclock() {
    pitlane::SimClock clock;
    clock.table.go_us[0] = 10;
    clock.table.pull_us[0] = 20;
    const int64_t morning = (9 * 60 + 40) * 60 * 1000000ll;
    expect(clock.go(morning) == 10 && clock.back(morning) == 20, "table delay");
    clock.pts[0] = pitlane::LagPoint{100, 0, 180};
    clock.pts[1] = pitlane::LagPoint{200, 0, 280};
    clock.npts = 2;
    expect(clock.go(150) < 0, "reject delay");
}

static void check_sample() {
    pitlane::SampleBox<8> box;
    expect(box.add(1) && box.add(3) && box.add(2), "samples");
    int64_t mean = 0;
    int64_t pct = 0;
    expect(box.mean(mean) && mean == 2 && box.min() == 1 && box.max() == 3, "mean");
    expect(box.pct(0, 100, pct) && pct == 1, "low rank");
    expect(box.pct(100, 100, pct) && pct == 3, "high rank");
    expect(box.pct(1, 2, pct) && pct == 2, "middle rank");
    expect(box.add(-1) && box.count() == 4, "negative sample");
    pitlane::SampleBox<2> tiny;
    expect(tiny.add(INT64_MAX) && !tiny.add(1) && tiny.count() == 1, "sum would overflow");
}

static void check_trace() {
    pitlane::LatTrace<4> trace;
    expect(pitlane::lat_mark(trace, 0, 10, false), "marking off");
    int64_t span = 0;
    expect(!pitlane::lat_span(trace, 0, 1, span), "unset");
    expect(pitlane::lat_mark(trace, 0, 10, true) && pitlane::lat_mark(trace, 1, 15, true), "marks");
    expect(pitlane::lat_span(trace, 0, 1, span) && span == 5, "span");
    pitlane::DeskSkew skew;
    int64_t revised = 0;
    expect(pitlane::skew_sample(skew, 1000, 1004) && pitlane::skew_apply(skew, 1000, revised) && revised == 1002, "half the round trip");
    expect(pitlane::skew_sample(skew, 1000, 1001) && skew.skew == 0, "odd millisecond");

    pitlane::LatHist<8> hist;
    expect(hist.add(0) && hist.add(1) && hist.add(2) && hist.add(4) && !hist.add(-1), "buckets");
    expect(hist.at(0) == 2 && hist.at(1) == 1 && hist.at(2) == 1 && hist.cum(1) == 3, "cumulative");
}

static void check_cut() {
    expect(pitlane::pick_cut(500, 1000, true, 2) == pitlane::CutChoice::KeepSolo, "solo");
    expect(pitlane::pick_cut(2000, 1000, false, 8) == pitlane::CutChoice::FanoutStateless, "fanout");
    expect(pitlane::pick_cut(2000, 1000, true, 2) == pitlane::CutChoice::SplitStateful, "split");
    const uint64_t arrivals[] = {0, 10, 20};
    uint64_t out[3]{};
    expect(pitlane::tandem_lat(arrivals, 3, 30, 2, 1, out, 3) == 3 && out[0] == 31, "tandem");
}

static void check_tsc() {
    pitlane::TscClock clock;
    expect(clock.calibrate(200), "tsc calibrate");
    expect(clock.ready(), "tsc ready");
    int64_t a = 0;
    int64_t b = 0;
    expect(clock.now_us(a) && clock.now_us(b) && b >= a, "tsc now");
    uint64_t ns = 0;
    expect(clock.now_ns(ns) && ns >= static_cast<uint64_t>(clock.base_ns()), "tsc now ns");

    pitlane::UsSlot slot{};
    expect(clock.publish(slot), "tsc publish");
    expect(pitlane::us_load(slot) >= a, "tsc published value");

    pitlane::TscStamp stamp{};
    expect(pitlane::tsc_stamp(stamp) && stamp.us > 0 && stamp.tsc > 0, "tsc stamp");
}

static void check_tail() {
    pitlane::TailBoard board{};
    board.budget_cycles = 100;
    expect(!pitlane::tail_note(board, pitlane::TailLane::Intent, 40), "tail below budget");
    expect(!pitlane::tail_note(board, pitlane::TailLane::Intent, 120), "tail over budget");
    expect(board.lane[0].notes == 2 && board.lane[0].stalls == 1 && board.lane[0].max_cycles == 120,
           "tail lane counters");
    pitlane::LatReport report{};
    expect(pitlane::tail_report(board, pitlane::TailLane::Intent, report) && report.n == 2 && report.max == 120,
           "tail report");
    expect(pitlane::tail_gap(board, pitlane::TailLane::Intent, 7), "tail gap");

    pitlane::tail_arm(board, 100, 1u << static_cast<int>(pitlane::TailLane::Intent));
    expect(pitlane::tail_note(board, pitlane::TailLane::Intent, 101), "tail trigger");
    expect(!pitlane::tail_note(board, pitlane::TailLane::Intent, 300), "tail trigger once");
    expect(pitlane::tail_fired(board) && board.trigger.at_cycles.load() == 101, "tail fired state");
    pitlane::tail_reset(board);
    expect(!pitlane::tail_fired(board) && board.lane[0].notes == 0, "tail reset");

    pitlane::TailTape<4> tape;
    tape.note(1, 10, 100);
    tape.note(1, 11, 101);
    tape.note(2, 12, 102);
    expect(tape.count() == 3 && !tape.frozen(), "tape notes");
    pitlane::TailTape<4>::Rec rec{};
    expect(tape.at(0, rec) && rec.tsc == 12 && rec.value == 102, "tape newest first");
    expect(tape.at(2, rec) && rec.tsc == 10, "tape oldest");
    expect(tape.freeze() && tape.frozen(), "tape freeze");
    tape.note(1, 13, 103);
    expect(tape.written() == 3 && tape.count() == 3, "tape frozen");
}

static void check_replay_pace() {
    pitlane::ReplayPace p{};
    p.reset();
    p.start(0, 1000000);
    expect(p.due(1000000) == 2000000, "pace 1x");
    expect(p.due(2500000) == 3500000, "pace forward");
    expect(p.due(500000) == 1500000, "pace rewind");
    expect(p.wait(2000000, 2500000) == 500000, "pace wait");
    p.reset();
    p.set_speed(pitlane::ReplayPace::kRealtime);
    expect(p.add_segment(0, 1000000, 40000000), "pace segment");
    expect(!p.add_segment(500000, 1500000, 40000000), "pace overlap");
    p.start(0, 0);
    expect(p.due(1000000) == 25000, "pace 40x");
    expect(p.due(2000000) == 1025000, "pace after segment");
    expect(p.speed_at(0) == 40000000, "pace speed at");
    expect(p.speed_at(1000000) == 1000000, "pace speed default");
    expect(p.in_break(500000) && !p.in_break(1500000), "pace break");
}

static void check_hops() {
    pitlane::HopLedger led;
    led.set_budget(pitlane::kHopDecode, 100);
    led.set_total_budget(300);
    expect(led.note(pitlane::kHopDecode, 50), "hop note");
    expect(led.note(pitlane::kHopDecode, 150), "hop note over");
    expect(!led.note(pitlane::kHopCount, 1), "hop stage bounds");
    pitlane::HopStageReport r{};
    expect(led.report(pitlane::kHopDecode, r), "hop report");
    expect(r.n == 2 && r.over == 1 && r.p99_ns >= r.p50_ns, "hop stats");
    expect(led.note_total(400) && led.total_over() == 1, "hop total budget");

    pitlane::HopTrace<8> t{};
    expect(pitlane::hop_trace_mark(t, pitlane::kHopWireIn, 1000), "trace mark in");
    expect(pitlane::hop_trace_mark(t, pitlane::kHopDecode, 1200), "trace mark decode");
    expect(pitlane::hop_trace_mark(t, pitlane::kHopBook, 1500), "trace mark book");
    pitlane::HopLedger led2;
    expect(pitlane::hop_trace_flush(t, led2, pitlane::kHopWireIn, pitlane::kHopBook) == 2, "trace flush");
    pitlane::HopStageReport dec{};
    expect(led2.report(pitlane::kHopDecode, dec) && dec.n == 1 && dec.p50_ns >= 192 && dec.p50_ns <= 200,
           "trace decode span");
}

static void check_baseline() {
    pitlane::PerfBaseline base;
    pitlane::LatReport r{};
    r.n = 10;
    r.p50 = 100;
    r.p99 = 200;
    r.p999 = 300;
    r.max = 400;
    expect(base.put("match", r), "baseline put");
    pitlane::MagHist h;
    for (int i = 0; i < 8; ++i) (void)h.add(static_cast<uint64_t>(100 + i * 10));
    expect(base.put("risk", h), "baseline hist put");

    char text[512];
    const int n = base.to_text(text, sizeof text, "abc123", 42);
    expect(n > 0 && std::strstr(text, "match") != nullptr, "baseline text");

    pitlane::PerfBaseline copy;
    expect(copy.from_text(text, n), "baseline parse");
    expect(copy.count() == 2 && copy.find("match") >= 0, "baseline roundtrip");
    const pitlane::PerfLine& m = copy.at(copy.find("match"));
    expect(m.p99 == 200 && m.n == 10, "baseline values");

    pitlane::PerfBaseline cur;
    cur.put("match", r);
    pitlane::PerfDiff diff[4]{};
    expect(base.compare(cur, 0, diff, 4) == 0, "baseline same");
    pitlane::LatReport slow = r;
    slow.p99 = 260;
    cur.put("match", slow);
    expect(base.compare(cur, 400000, diff, 4) == 0, "baseline within tolerance");
    expect(base.compare(cur, 100000, diff, 4) == 1 && diff[0].regressed == 1, "baseline regression");
    expect(pitlane::PerfBaseline::delta_ppm_of(200, 260) == 300000, "baseline delta ppm");
}

static void check_flame() {
    pitlane::FlameBook fb;
    const int a = fb.enter("tick", 1000);
    const int b = fb.enter("decode", 1100);
    fb.leave(b, 1300);
    const int c = fb.enter("match", 1300);
    fb.leave(c, 1700);
    fb.leave(a, 1700);
    char out[512];
    const int n = fb.folded(out, sizeof out);
    expect(n > 0, "flame folded");
    expect(std::strstr(out, "tick;decode ") != nullptr, "flame child path");
    expect(std::strstr(out, "tick;match ") != nullptr, "flame second child");
    expect(fb.nodes() == 3 && fb.depth() == 0, "flame nodes");
}

int main() {
    check_cycles();
    check_hist_and_laps();
    check_clock();
    check_wheel();
    check_span();
    check_tape();
    check_simclock();
    check_sample();
    check_trace();
    check_cut();
    check_tsc();
    check_tail();
    check_replay_pace();
    check_hops();
    check_baseline();
    check_flame();
    if (g_fails) {
        std::fprintf(stderr, "%d time check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
