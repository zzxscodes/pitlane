#include "pitlane/pitlane.hpp"

#include <cstdio>

// Cycle scale, a published clock, a timer wheel, a histogram, and an in-place ring.

int fail(const char* what) {
    std::fprintf(stderr, "time: %s\n", what);
    return 1;
}

int main() {
    uint64_t t0 = 0;
    uint64_t t1 = 0;
    if (!pitlane::cycles_ordered(t0)) return fail("cycles");
    unsigned char pad[128]{};
    pitlane::touch_lines(pad, sizeof pad);
    if (!pitlane::cycles_ordered(t1) || t1 < t0) return fail("order");

    pitlane::CycleScale scale{};
    if (!pitlane::scale_set(scale, 1000, 500)) return fail("scale");
    uint64_t ns = 0;
    if (!pitlane::scale_ns(scale, 1000, ns) || ns != 500) return fail("ns");

    pitlane::ClockPage page{};
    if (!pitlane::clock_publish(page, 1000, 500, 1000, 10, 0)) return fail("clock");
    pitlane::ClockView view{};
    int64_t sec = 0;
    int64_t nsec = 0;
    if (!view.refresh(page) || !view.to_wall(1000, sec, nsec)) return fail("wall");

    pitlane::TickWheel<16> wheel;
    if (!wheel.arm(1, 7)) return fail("arm");
    uint32_t due[4]{};
    if (wheel.take(1, due, 4) != 1 || due[0] != 7) return fail("due");

    pitlane::MagHist hist;
    if (!hist.add(80) || !hist.add(120)) return fail("hist");
    pitlane::LatReport report{};
    if (!pitlane::mag_report(hist, report) || report.n != 2) return fail("report");

    pitlane::SpanMark<4> span;
    int64_t gap = 0;
    if (!span.mark(0, 10) || !span.mark(1, 25) || !span.between(0, 1, gap) || gap != 15) return fail("span");

    pitlane::FillRing<int, 4> ring;
    int* slot = ring.claim();
    if (!slot) return fail("claim");
    *slot = 5;
    if (!ring.publish()) return fail("publish");
    int got = 0;
    if (!ring.take(got) || got != 5) return fail("take");
    int* again = ring.claim();
    if (!again) return fail("claim again");
    *again = 6;
    if (!ring.publish()) return fail("publish again");
    again = ring.claim();
    if (!again) return fail("claim last");
    *again = 7;
    if (!ring.publish()) return fail("publish last");
    int batch[2]{};
    if (ring.take_span(batch, 2) != 2 || batch[0] != 6 || batch[1] != 7) return fail("span");

    uint8_t storage[16]{};
    pitlane::ByteFrame frame;
    const char body[] = {'a', 'b'};
    if (!frame.begin(storage, 16, 1) || !frame.put(body, 2) || frame.end() != 6) return fail("frame");

    std::printf("time ns=%llu wall=%lld.%09lld due=%u gap=%lld\n", static_cast<unsigned long long>(ns),
                static_cast<long long>(sec), static_cast<long long>(nsec), due[0], static_cast<long long>(gap));
    return 0;
}
