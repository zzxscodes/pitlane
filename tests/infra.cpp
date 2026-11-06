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

void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

int make_sum(uint8_t* dst, const char* body, int n, bool good) {
    put_u32(dst, static_cast<uint32_t>(n));
    if (n > 0) std::memcpy(dst + 4, body, static_cast<std::size_t>(n));
    pitlane::digest_write(pitlane::FrameDigest::Sum8, dst, 4 + n, dst + 4 + n);
    if (!good) dst[4 + n] = static_cast<uint8_t>(dst[4 + n] ^ 0xffu);
    return 5 + n;
}

void check_stream() {
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    expect(pitlane::digest_of(pitlane::FrameDigest::Crc32, digits, 9) == 0xCBF43926u, "crc32");
    expect(pitlane::digest_of(pitlane::FrameDigest::Crc16, digits, 9) == 0x29B1u, "crc16");
    const uint8_t bytes[] = {1, 2, 3};
    expect(pitlane::digest_of(pitlane::FrameDigest::Sum8, bytes, 3) == 6, "sum8");

    uint8_t raw[64]{};
    const int bad_n = make_sum(raw, "XY", 2, false);
    const int good_n = make_sum(raw + bad_n, "Z", 1, true);
    pitlane::StreamAsm<64> stream;
    pitlane::FrameSpec spec;
    spec.max_body = 8;
    expect(stream.append(raw, 2) && stream.pull(spec, nullptr, nullptr) == pitlane::AsmCode::BadArg, "short header needs arguments");
    const uint8_t* frame = nullptr;
    int len = 0;
    expect(stream.pull(spec, &frame, &len) == pitlane::AsmCode::Need, "header not finished");
    expect(stream.append(raw + 2, bad_n + good_n - 2), "rest of the bytes");
    expect(stream.pull(spec, &frame, &len) == pitlane::AsmCode::BadDig, "bad checksum is dropped");
    expect(stream.pull(spec, &frame, &len) == pitlane::AsmCode::Frame && len == good_n && frame[4] == 'Z', "next frame remains");
    expect(!stream.append(raw, 1), "frame still held");
    stream.commit();
    expect(stream.stored() == 0, "committed");

    uint8_t huge[8]{};
    put_u32(huge, 1000);
    pitlane::StreamAsm<64> stuck;
    expect(stuck.append(huge, 4), "declared length");
    const int before = stuck.stored();
    expect(stuck.pull(spec, &frame, &len) == pitlane::AsmCode::BadLen && stuck.stored() == before, "over the limit stays");
    expect(stuck.pull(spec, &frame, &len) == pitlane::AsmCode::BadLen, "still stuck");
}

void check_gate_pace() {
    pitlane::SeqGate<4> gate;
    expect(!gate.start(0) && gate.start(1), "start at one");
    uint64_t out[8]{};
    expect(gate.live(1, out, 8) == 1 && out[0] == 1, "first live");
    expect(gate.live(4, out, 8) == 0 && gate.recovering() && gate.parked() == 1, "gap parks the later number");
    uint64_t lo = 0;
    uint64_t hi = 0;
    expect(gate.missing(lo, hi) && lo == 2 && hi == 3, "missing range");
    expect(gate.live(4, out, 8) == 0 && gate.parked() == 1, "duplicate park");
    expect(gate.replay(3, out, 8) == -1 && gate.last() == 1, "replay must start at the gap");
    expect(gate.replay(2, out, 8) == 1 && out[0] == 2, "fill two");
    expect(gate.replay(3, out, 1) == -4 && gate.last() == 2, "short output changes nothing");
    expect(gate.replay(3, out, 8) == 2 && out[0] == 3 && out[1] == 4 && !gate.recovering(), "drain the side list");

    pitlane::SeqGate<2> small;
    expect(small.start(1), "small start");
    expect(small.live(3, out, 8) == 0 && small.live(5, out, 8) == 0 && small.live(7, out, 8) == -2, "side list full");
    expect(small.parked() == 2 && small.missing(lo, hi) && lo == 1 && hi == 2, "gap unchanged");
    expect(small.replay(1, out, 8) == 1, "replay one");
    expect(small.replay(2, out, 8) == 2 && out[1] == 3 && small.recovering(), "next hole");
    expect(small.missing(lo, hi) && lo == 4 && hi == 4, "hole before five");
    expect(small.replay(4, out, 8) == 2 && out[1] == 5 && !small.recovering(), "caught up");

    pitlane::TokenPot pot;
    expect(pitlane::token_open(pot, 10, 1, 1, 0) && pitlane::token_take(pot, 0, 10), "spend the burst");
    expect(!pitlane::token_take(pot, 0, 1), "empty");
    expect(pitlane::token_take(pot, 5, 5) && !pitlane::token_take(pot, 4, 1), "clock back does not refill");

    pitlane::GcraPot gcra;
    expect(pitlane::gcra_open(gcra, 10, 10) && pitlane::gcra_allow(gcra, 0) && pitlane::gcra_allow(gcra, 0), "two early");
    expect(!pitlane::gcra_allow(gcra, 0) && pitlane::gcra_allow(gcra, 10), "after the interval");

    pitlane::TokenBook<1> book;
    expect(book.open(5, 1, 1, 10) && book.take(1, 0, 1), "key one");
    expect(!book.take(2, 5, 1) && book.take(2, 10, 1), "idle slot");
    expect(!book.take(1, 10, 1), "old key was reused");
}

void check_sign_ack() {
    pitlane::SignSlip slip;
    slip.need = 0;
    expect(pitlane::sign_ready(slip), "no signer required");
    slip.need = 2;
    expect(pitlane::sign_pass(slip) && !pitlane::sign_ready(slip) && pitlane::sign_pass(slip) && pitlane::sign_ready(slip), "two passes");
    expect(pitlane::sign_reject(slip) && !pitlane::sign_pass(slip), "reject sticks");
    pitlane::sign_reset(slip);
    expect(pitlane::sign_pass(slip) && !pitlane::sign_ready(slip), "need remains");

    pitlane::OpenQuota quota{10, 2};
    pitlane::OpenHold hold{};
    expect(pitlane::hold_reserve(quota, hold, true, 4) && quota.lots == 6 && quota.cancels == 1, "open holds a cancel");
    expect(pitlane::hold_cancel(quota, hold, 1, false) && quota.lots == 9 && quota.cancels == 1 && !hold.on, "cancel reached the venue");
    expect(pitlane::hold_reserve(quota, hold, true, 4) && quota.lots == 5 && quota.cancels == 0, "second open");
    expect(pitlane::hold_done(quota, hold, 4, true) && quota.cancels == 0 && !hold.on, "outside fill keeps the credit");
    expect(!pitlane::hold_reserve(quota, hold, true, 4), "no cancel credit left");
    quota.cancels = 1;
    expect(pitlane::hold_reserve(quota, hold, true, 4) && quota.lots == 1 && quota.cancels == 0, "third open");
    expect(pitlane::hold_done(quota, hold, 4, false) && quota.cancels == 1, "full fill returns the credit");
}

void check_sign_rest() {
    pitlane::OpenQuota quota{10, 2};
    pitlane::OpenHold hold{};
    expect(pitlane::hold_reserve(quota, hold, true, 4), "reserve");
    expect(pitlane::hold_cancel(quota, hold, 0, true) && quota.lots == 10 && quota.cancels == 2 && !hold.on, "reject never sent");
    expect(pitlane::hold_reserve(quota, hold, false, 3) && quota.lots == 10 && quota.cancels == 2, "close holds nothing");
    expect(pitlane::hold_late(quota, hold, 0) && quota.cancels == 2 && !hold.on, "close timeout");
    expect(pitlane::hold_reserve(quota, hold, true, 4) && quota.lots == 6 && quota.cancels == 1, "open again");
    expect(pitlane::hold_cancel_late(quota, hold) && hold.on && quota.cancels == 2 && !hold.cancel_held, "cancel timeout");
    expect(pitlane::hold_done(quota, hold, 4, false) && quota.cancels == 2, "later fill does not return it twice");
    expect(pitlane::hold_reserve(quota, hold, true, 2), "last open");
    expect(pitlane::hold_late(quota, hold, 0) && quota.lots == 6 && quota.cancels == 2, "open timeout returns both");

    pitlane::AckBook<2> acks;
    pitlane::VenueKey num{};
    pitlane::VenueKey text{};
    expect(pitlane::key_num(num, 9, "ctp") && pitlane::key_text(text, "ABC", "ctp") && !pitlane::key_same(num, text), "different key kinds");
    expect(!pitlane::key_text(num, "this name is longer than the field", "ctp") && num.num == 9, "long text keeps the old key");
    expect(acks.put(num, 11, 0) && acks.put(text, 12, 0) && !acks.put(num, 13, 1), "full book");
    pitlane::AckClock clock;
    clock.place_lim = 10;
    clock.cancel_lim = 5;
    clock.gap = 100;
    uint64_t ids[4]{};
    expect(acks.sweep(clock, 10, ids, 4) == 0, "not due yet");
    expect(acks.sweep(clock, 11, ids, 4) == 0, "inside the gap");
    expect(acks.sweep(clock, 111, ids, 4) == 2, "both place timers");
    expect(acks.link(num) && acks.arm_cancel(num, 111), "cancel armed");
    expect(acks.sweep(clock, 111, ids, 4) == 0, "gap after the sweep");
    expect(acks.sweep(clock, 220, ids, 4) == 2, "place and cancel");
    expect(!acks.link(num), "row was not cleared");
}

void check_trace_snap() {
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

    struct Sample {
        int v{0};
    };
    pitlane::SnapPub<Sample, 2> two;
    Sample* first = two.begin_write();
    first->v = 1;
    expect(two.publish(), "publish");
    const Sample* held = two.pin();
    expect(held && held->v == 1, "pin");
    Sample* second = two.begin_write();
    second->v = 2;
    expect(two.publish() && two.begin_write() == nullptr && held->v == 1, "both slots busy");
    two.unpin();
    Sample* third = two.begin_write();
    third->v = 3;
    expect(two.publish(), "after unpin");
    const Sample* again = two.pin();
    expect(again && again->v == 3, "latest");

    pitlane::SnapPub<Sample, 3> three;
    Sample* s0 = three.begin_write();
    s0->v = 1;
    expect(three.publish(), "three first");
    const Sample* old = three.pin();
    Sample* s1 = three.begin_write();
    s1->v = 2;
    expect(three.publish(), "three middle");
    Sample* s2 = three.begin_write();
    s2->v = 3;
    expect(three.publish() && old->v == 1, "unread middle can be replaced");
    three.unpin();
    const Sample* latest = three.pin();
    expect(latest && latest->v == 3, "reader moves to the latest");
}

void check_sig_grace() {
    pitlane::SigBook<2, 20> book;
    uint8_t key[17][16]{};
    for (int i = 0; i < 16; ++i) {
        key[i][0] = static_cast<uint8_t>(i + 1);
        expect(book.put(key[i]), "insert");
    }
    expect(book.live() == 16, "full");
    key[16][0] = 99;
    expect(!book.put(key[16]) && book.live() == 16, "seventeenth fails");
    for (int i = 0; i < 16; ++i) expect(book.find(key[i]) > 0, "old key remains");
    expect(book.put(key[0]) && book.live() == 16, "duplicate");

    pitlane::Grace<2> grace;
    grace.online(0);
    grace.online(1);
    const uint64_t token = grace.bump();
    expect(!grace.elapsed(token), "readers are behind");
    grace.quiesce(0);
    expect(!grace.elapsed(token), "one reader remains");
    grace.quiesce(1);
    expect(grace.elapsed(token), "both moved on");
    grace.offline(1);
    const uint64_t next = grace.bump();
    grace.quiesce(0);
    expect(grace.elapsed(next), "offline reader does not block");

    pitlane::Grace<1> one;
    one.online(0);
    pitlane::RetireList<2> retired;
    const uint64_t pending = one.bump();
    expect(retired.push(pending, 7), "shelve");
    uint32_t user[2]{};
    expect(retired.reap(one, user, 2) == 0 && retired.size() == 1, "not yet");
    one.quiesce(0);
    expect(retired.reap(one, user, 0) == -1 && retired.size() == 1, "short output");
    expect(retired.reap(one, user, 2) == 1 && user[0] == 7 && retired.size() == 0, "reaped");
}

}  // namespace

int main() {
    check_stream();
    check_gate_pace();
    check_sign_ack();
    check_sign_rest();
    check_trace_snap();
    check_sig_grace();
    if (g_fails) {
        std::fprintf(stderr, "%d infra check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
