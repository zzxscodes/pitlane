#include "pitlane/pitlane.hpp"

#include <cstdio>
#include <cstring>

namespace {

int g_fails = 0;

void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

void check_hex_mix_text() {
    const uint8_t raw[] = {0x0a, 0xb0, 0x03};
    char hex[16]{};
    expect(pitlane::hex_encode(raw, 3, hex, 16) == 6 && std::strcmp(hex, "0ab003") == 0, "encode");
    expect(pitlane::hex_encode(raw, 3, hex, 6) == -1 && std::strcmp(hex, "0ab003") == 0, "short encode keeps the old text");
    uint8_t back[4]{};
    expect(pitlane::hex_decode("0AB003", 6, back, 4) == 3 && back[0] == 0x0a && back[1] == 0xb0 && back[2] == 0x03, "decode");
    expect(pitlane::hex_decode("0ab00", 5, back, 4) == -1 && back[2] == 0x03, "odd length");
    expect(pitlane::hex_decode("0ag0", 4, back, 4) == -1, "bad digit");
    const uint8_t other[] = {0x0a, 0xb0, 0x04};
    expect(pitlane::bytes_diff(raw, raw, 3) == -1, "same bytes");
    expect(pitlane::bytes_diff(raw, other, 3) == 2, "third byte");

    pitlane::Mix64 mix;
    expect(mix.next() == 0xe220a8397b1dcdafull, "first mix");
    expect(mix.next() == 0x6e789e6aa1b965f4ull, "second mix");
    pitlane::Mix64 span;
    int64_t got = 0;
    expect(span.span(5, 5, got) && got == 5, "one point");
    expect(span.span(4, 1, got) == false, "empty span");
    uint8_t bytes[3]{};
    pitlane::Mix64 fill;
    expect(fill.bytes(bytes, 3), "bytes");
    expect(bytes[0] == static_cast<uint8_t>(0xe220a8397b1dcdafull), "low byte");

    pitlane::TextBuf<16> text;
    expect(text.put("id=") && text.put_i64(-42) && text.put_char(' ') && text.put_hex(0x2a, 4), "build");
    expect(std::strcmp(text.c_str(), "id=-42 002a") == 0, "text");
    expect(!text.put("too-long-for-room") && std::strcmp(text.c_str(), "id=-42 002a") == 0, "full text stays");
    expect(text.put_i64(INT64_MIN) == false, "min does not fit");
    pitlane::TextBuf<32> wide;
    expect(wide.put_i64(INT64_MIN) && std::strcmp(wide.c_str(), "-9223372036854775808") == 0, "min int");
}

void check_sample_drill() {
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

    pitlane::StepClock clock;
    expect(clock.jump(10) && clock.step(5) && clock.now == 15, "step");
    expect(!clock.step(INT64_MAX) && clock.now == 15, "step overflow");
    expect(clock.jump(-3) && clock.now == -3, "jump back");

    pitlane::FailBudget budget;
    budget.arm(2);
    expect(budget.allow() && budget.allow() && !budget.allow(), "budget");

    pitlane::StepList<4> steps;
    expect(steps.push(10, 1) && steps.push(10, 2) && !steps.push(9, 3), "time order");
    uint64_t tags[4]{};
    expect(steps.take(9, tags, 4) == 0 && steps.take(10, tags, 1) == 1 && tags[0] == 1, "partial take");
    expect(steps.take(10, tags, 4) == 1 && tags[0] == 2 && steps.size() == 0, "rest");

    const int64_t bid_px[] = {10, 9};
    const int64_t bid_qty[] = {1, 2};
    const int64_t bad_px[] = {9, 10};
    const int64_t zero_qty[] = {1, 0};
    expect(pitlane::ladder_check(1, bid_px, bid_qty, 2) == pitlane::RowFault::None, "bid ladder");
    expect(pitlane::ladder_check(1, bad_px, bid_qty, 2) == pitlane::RowFault::Order, "bid order");
    expect(pitlane::ladder_check(2, bid_px, bid_qty, 2) == pitlane::RowFault::Order, "ask order");
    expect(pitlane::ladder_check(2, bad_px, bid_qty, 2) == pitlane::RowFault::None, "ask ladder");
    expect(pitlane::ladder_check(1, bid_px, zero_qty, 2) == pitlane::RowFault::Qty, "zero qty");
    expect(pitlane::ladder_check(1, bid_px, bid_qty, 0) == pitlane::RowFault::None, "empty ladder");

    pitlane::Intent intent{};
    expect(pitlane::intent_make(intent, 7, 3, 1, 100, 5, 9, 1, 1, 1, 10) && intent.tag == 7 && intent.lot == 10, "intent");
    expect(!pitlane::intent_make(intent, 7, 3, 1, 100, 5, 9, 0, 1, 1, 10) && intent.tag == 0, "bad side clears");

    pitlane::Report report{};
    expect(pitlane::report_make(report, 7, 8, 3, 100, 2, 3, 9, 0, 3, 1) && report.phase == 3 && report.left == 3, "report");
    expect(!pitlane::report_make(report, 7, 0, 3, -1, 0, 0, 0, 0, 0, 0) && report.tag == 0, "bad price");

    pitlane::Quote quote{};
    expect(pitlane::quote_make(quote, 3, 1, 2, 10, 11, 10, 4, 5, 1) && quote.ask_px == 11, "quote");
    expect(!pitlane::quote_make(quote, 3, 1, 2, 12, 11, 10, 4, 5, 1) && quote.name == 0, "crossed quote");
}

}  // namespace

int main() {
    check_hex_mix_text();
    check_sample_drill();
    if (g_fails) {
        std::fprintf(stderr, "%d drill check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
