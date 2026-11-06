#include "pitlane/pitlane.hpp"

#include <cstdint>
#include <cstdio>

// Latency samples after a discarded warmup. The pass checks are the rank order,
// a longer pause having a higher median, and the median staying under a wide ceiling.
// A machine is not failed for its absolute nanosecond number.

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

    return g_fails == 0 ? 0 : 1;
}
