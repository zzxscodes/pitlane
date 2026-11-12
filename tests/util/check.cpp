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

static void check_numeric() {
    expect(pitlane::round_down_to(7, 5) == 5 && pitlane::round_up_to(7, 5) == 10, "round");
    expect(pitlane::round_nearest_to(-7, 5) == -5, "nearest");
    expect(pitlane::round_up_nonzero(0, 5) == 5, "nonzero");
    expect(pitlane::max_abs(-8, 3) == -8 && pitlane::min_abs(-8, 3) == 3, "abs");
    char hex[8]{};
    expect(pitlane::to_hex(0x2a, hex, 8) == 2 && std::strcmp(hex, "2a") == 0, "hex");
}

static void check_wide() {
    pitlane::Wide w = pitlane::mul_wide(10000000000ull, 10000000000ull);
    uint64_t q = 0;
    expect(pitlane::div_wide(w, 10000000000ull, q) && q == 10000000000ull, "wide divide");
}

static void check_text() {
    char parts[4][64];
    expect(pitlane::split_text("a,,b", ',', parts, 4, true) == 2, "drop empty");
    const char* join_in[] = {"aa", "bb"};
    char joined[16]{};
    expect(pitlane::join_text(join_in, 2, '|', joined, 16) && std::strcmp(joined, "aa|bb") == 0, "join");
    char padded[8]{};
    expect(pitlane::pad_left("7", '0', 3, padded, 8) && std::strcmp(padded, "007") == 0, "pad");
    expect(pitlane::contains_ci("AbC", "bc"), "ci");
    expect(pitlane::hash_str("pit") == pitlane::hash32("pit", 3), "hash");

    pitlane::FlatMap map;
    expect(pitlane::parse_flat_json("{\"k\":\"v\",\"n\":\"1\"}", &map) && map.n == 2, "json");
    expect(std::strcmp(pitlane::flat_get(map, "n"), "1") == 0, "json get");
    char json[64]{};
    expect(pitlane::write_flat_json(map, json, 64), "json write");
    pitlane::LagTable lags{};
    expect(pitlane::load_lag_csv("0,100,200\n1,300,400\n", &lags) && lags.go_us[1] == 300 && lags.pull_us[0] == 200, "lag csv");
    const char* path = "pitlane_lines.txt";
    expect(pitlane::write_text(path, "one\ntwo\n"), "write");
    char lines[4][160];
    int nlines = 0;
    expect(pitlane::read_lines(path, lines, 4, &nlines) && nlines == 2 && std::strcmp(lines[1], "two") == 0, "read");
    ::unlink(path);
    const unsigned char gbk[] = {0xd6, 0xd0};
    char utf[8]{};
    expect(pitlane::gbk_to_utf8(reinterpret_cast<const char*>(gbk), 2, utf, sizeof utf), "gbk");
    expect(static_cast<unsigned char>(utf[0]) == 0xe4 && static_cast<unsigned char>(utf[1]) == 0xb8 &&
               static_cast<unsigned char>(utf[2]) == 0xad,
           "utf8");
}

static void check_frame() {
    uint8_t buf[32]{};
    pitlane::ByteFrame frame;
    expect(frame.begin(buf, 32, 7), "begin");
    const char body[] = "ab";
    expect(frame.put(body, 2), "put");
    expect(frame.end() == 6, "end");
    pitlane::FrameView view{};
    expect(pitlane::frame_read(buf, 6, view) && view.kind == 7 && view.len == 2 && view.body[0] == 'a', "read");
    expect(!pitlane::frame_read(buf, 5, view), "truncated");

    pitlane::SeqWatch watch{};
    uint64_t missed = 0;
    expect(pitlane::seq_feed(watch, 5, &missed) == 0 && watch.next == 6, "first seq");
    expect(pitlane::seq_feed(watch, 8, &missed) == 2 && missed == 2, "gap");
    expect(pitlane::seq_feed(watch, 8, &missed) == -1, "old seq");

    pitlane::SeqHold<4, 8> hold;
    const uint8_t a[] = {1};
    const uint8_t b[] = {2};
    expect(hold.start(1), "start");
    expect(hold.offer(2, b, 1) && hold.offer(1, a, 1), "hold ahead");
    uint8_t out[8]{};
    uint64_t seq = 0;
    expect(hold.take(out, 8, &seq) == 1 && seq == 1 && out[0] == 1, "take first");
    expect(hold.take(out, 8, &seq) == 1 && seq == 2 && out[0] == 2, "take next");
    expect(hold.take(out, 8, &seq) == 0, "waiting");
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

static void check_drill() {
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

static void check_snap() {
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
    const uint8_t* batch[4] = {key[0], key[1], key[2], key[3]};
    int found[4] = {};
    book.find4(batch, found);
    expect(found[0] > 0 && found[1] > 0 && found[2] > 0 && found[3] > 0, "find4");

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

void check_send_join() {
    pitlane::SendJoinRing<uint64_t, 4> ring;
    expect(ring.try_push(10) && ring.try_push(20) && ring.try_push(30), "push");
    expect(!ring.try_push(40), "full");
    uint64_t out = 0;
    expect(ring.try_pop(out) && out == 10, "pop first");
    expect(ring.try_push(40), "after pop");
    expect(ring.try_pop(out) && out == 20, "pop second");
    expect(ring.try_pop(out) && out == 30, "pop third");
    expect(ring.try_pop(out) && out == 40, "pop fourth");
    expect(!ring.try_pop(out), "empty");
}

void check_stage_seq_fill() {
    pitlane::StageRing<uint64_t, 4> ring;
    uint32_t a = 0;
    uint32_t b = 0;
    expect(ring.acquire(a) && a == 0 && ring.acquire(b) && b == 1, "claim");
    expect(ring.finish(b, 0, 20), "later finish");
    uint32_t seq = 0;
    uint64_t body = 0;
    expect(!ring.take(0, seq, body), "blocked");
    expect(ring.finish(a, 0, 10), "earlier finish");
    expect(ring.take(0, seq, body) && seq == 0 && body == 10, "first");
    expect(ring.take(0, seq, body) && seq == 1 && body == 20, "second");
    uint32_t extra[4]{};
    int n = 0;
    while (n < 4 && ring.acquire(extra[n])) ++n;
    expect(n == 2, "two more claims");
    uint32_t overflow = 0;
    expect(!ring.acquire(overflow), "full stage");

    pitlane::SeqWin<8> win;
    expect(win.push(0, 1) == pitlane::SeqPut::Bad, "empty token");
    expect(win.push(10, 1) == pitlane::SeqPut::In, "first");
    expect(win.push(30, 3) == pitlane::SeqPut::In, "hole");
    uint64_t out[4]{};
    expect(win.pull(out, 4) == 1 && out[0] == 10, "stop at hole");
    expect(win.push(20, 2) == pitlane::SeqPut::In, "fill hole");
    expect(win.pull(out, 4) == 2 && out[0] == 20 && out[1] == 30, "released");
    expect(win.push(11, 1) == pitlane::SeqPut::Far, "behind");
    expect(win.push(10, 4) == pitlane::SeqPut::In && win.push(10, 4) == pitlane::SeqPut::Dup, "duplicate");

    pitlane::FillRing<uint64_t, 2> fill;
    uint64_t* slot = fill.claim();
    expect(slot != nullptr, "claim");
    *slot = 5;
    expect(fill.claim() == nullptr, "one claim");
    expect(fill.publish(), "publish");
    uint64_t got = 0;
    expect(fill.take(got) && got == 5, "took");
    slot = fill.claim();
    *slot = 1;
    fill.cancel();
    slot = fill.claim();
    expect(slot != nullptr, "cancelled claim");
    *slot = 2;
    expect(fill.publish() && fill.take(got) && got == 2, "rewritten");
}

static void check_readers() {
    pitlane::ReadWin readers;
    expect(!readers.open(3) && readers.open(128), "window");
    expect(readers.join(0, 0, true) && readers.join(1, 0, false), "join");
    uint64_t lim = 0;
    expect(readers.limit(100, 1, 10, lim) && lim == 128, "slowest");
    expect(readers.joined(1), "still in");
    expect(readers.report(0, 80), "move");
    expect(readers.limit(100, 20, 10, lim) && !readers.joined(1) && lim == 208, "quiet reader leaves");

    alignas(64) char block[256]{};
    void* at = nullptr;
    expect(pitlane::core_slot(block, 1, 4, 64, at) && at == block + 64, "stride");
    expect(!pitlane::core_slot(block, 1, 4, 32, at), "short stride");
}

static void check_seqpub() {
    pitlane::SeqPublishRing<int, 4> ring;
    ring.publish([](int& v) { v = 1; });
    ring.publish([](int& v) { v = 2; });
    int64_t cur = 0;
    int sum = 0;
    cur = ring.drain_from(cur, [&](const int& v) { sum += v; });
    expect(cur == 2 && sum == 3, "seqpub");
    for (int i = 0; i < 8; ++i) ring.publish([i](int& v) { v = i + 10; });
    sum = 0;
    cur = ring.drain_from(cur, [&](const int& v) { sum += v; });
    expect(cur == 10 && sum > 0, "overwrite drain");
}

static void check_global_scale() {
    expect(pitlane::global_scale_set(1000, 500), "global set");
    uint64_t ns = 0;
    expect(pitlane::global_scale_ns(1000, ns) && ns == 500, "global ns");
    pitlane::global_scale_clear();
    expect(!pitlane::global_scale_on(), "global clear");
}

static void check_simd() {
    alignas(16) int64_t words[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    pitlane::zero_i64(words, 8);
    expect(words[0] == 0 && words[7] == 0, "zero");
    const int64_t src[4] = {9, 8, 7, 6};
    int64_t dst[4] = {};
    pitlane::copy_bytes(dst, src, sizeof src);
    expect(dst[0] == 9 && dst[3] == 6, "copy");
    expect(pitlane::bytes_eq(src, dst, sizeof src), "eq");
    expect(pitlane::kSimdI64 >= 1 && pitlane::kUnroll4 == 4, "widths");
    expect(pitlane::likely(true) && !pitlane::unlikely(false), "hints");
    pitlane::prefetch_lines(dst, sizeof dst);
    alignas(16) int64_t big[64];
    for (int i = 0; i < 64; ++i) big[i] = i + 1;
    pitlane::zero_i64_stream(big, 64);
    expect(big[0] == 0 && big[63] == 0, "stream zero");
}

static void check_bound_spin_arena() {
    expect(pitlane::index_ok(0, 4) && !pitlane::index_ok(-1, 4) && !pitlane::index_ok(4, 4), "index");
    expect(pitlane::range_ok(5, 1, 9) && !pitlane::range_ok(0, 1, 9), "range");
    expect(pitlane::wrap_pow2(17, 16) == 1, "wrap");
    pitlane::WalkBudget budget(8);
    pitlane::Walk walk{&budget, 4};
    expect(walk.enter(3) && walk.enter(3) && !walk.enter(3), "walk");
    pitlane::SpinLock lock;
    expect(lock.try_lock() && !lock.try_lock(), "spin try");
    lock.unlock();
    lock.lock();
    lock.unlock();
    pitlane::ByteArena arena;
    pitlane::ArenaOpt opt{};
    opt.hugepage = 0;
    opt.lock = 0;
    opt.prefault = 1;
    expect(arena.open(4096, opt), "arena open");
    void* a = arena.take(64, 64);
    void* b = arena.take(32, 8);
    expect(a && b && arena.used() >= 96, "arena take");
    expect(arena.clear() && arena.used() == 0, "arena clear");
    arena.close();

    pitlane::ByteArena huge;
    expect(huge.open(4096), "arena default open");
    expect(huge.capacity() >= 4096, "arena default cap");
    expect(huge.take(16, 8) != nullptr, "arena default take");
    huge.close();

    expect(pitlane::page_size_bytes() >= 4096u, "page size");
    expect(pitlane::hugepage_size_bytes() >= pitlane::page_size_bytes(), "huge size");
    expect(pitlane::round_up_bytes(1, 4096) == 4096, "round bytes");

    expect(pitlane::batch_size_for(0) == 1, "batch low");
    expect(pitlane::batch_size_for(20) == 32, "batch mid");
    expect(pitlane::batch_size_for(100) == 256, "batch high");
    expect(pitlane::batch_size_capped(100, 10) == 10, "batch cap");

    pitlane::RankBits<2> ranks;
    const uint32_t ids[] = {1, 3, 65};
    expect(ranks.build(ids, 3) && ranks.live() == 3, "rank build");
    expect(ranks.find(1) == 0 && ranks.find(3) == 1 && ranks.find(65) == 2, "rank find");
    expect(ranks.find(2) < 0, "rank miss");

    pitlane::QuadTab<4> tab;
    const uint64_t keys[] = {10, 20, 30, 40, 50};
    expect(tab.seat(keys, 5), "quad seat");
    expect(tab.find(10) >= 0 && tab.find(50) >= 0 && tab.find(99) < 0, "quad find");

    pitlane::ObjectPool<int, 4> pool;
    int* pa = pool.allocate();
    int* pb = pool.allocate();
    expect(pa && pb && pool.live() == 2, "pool alloc");
    *pa = 1;
    *pb = 2;
    expect(pool.release(pa) && pool.live() == 1, "pool free");
    int* pc = pool.allocate();
    expect(pc == pa && pool.free() == 2, "pool reuse");

    pitlane::StickyCounter sticky(1);
    expect(sticky.read() == 1, "sticky read");
    expect(sticky.decrement(), "sticky zero");
    expect(sticky.read() == 0 && !sticky.increment_if_not_zero(), "sticky sticky");

    pitlane::CoreBank<2, 128> bank;
    bank.reset();
    int* h = static_cast<int*>(bank.alloc(sizeof(int), alignof(int)));
    expect(h != nullptr, "bank alloc");
    *h = 7;
    int* other = static_cast<int*>(bank.at(h, 1));
    expect(other && *other == 0, "bank other");
    *other = 9;
    expect(*h == 7 && *other == 9, "bank split");

    pitlane::WirePx w{};
    w.m = 12345;
    w.s = -2;
    int64_t eng = 0;
    expect(pitlane::to_engine(w, 1, eng) && eng == 123, "wire to engine");
    pitlane::WirePx back{};
    expect(pitlane::from_engine(12300, -2, back) && back.m == 1230000, "engine to wire");
    int64_t prod = 0;
    expect(pitlane::fixed_mul(100, 1, 200, 1, 1, prod) && prod == 20000, "fixed mul");
}

static void check_touch() {
    unsigned char buf[128]{};
    buf[0] = 1;
    buf[127] = 2;
    pitlane::touch_lines(buf, sizeof buf);
    expect(buf[0] == 1 && buf[127] == 2, "touch");

    pitlane::FillRing<int, 2> ring;
    int* slot = ring.claim();
    expect(slot != nullptr, "claim");
    *slot = 7;
    expect(ring.publish(), "pub");
    slot = ring.claim();
    *slot = 8;
    expect(ring.publish() && ring.claim() == nullptr, "full");
    int got = 0;
    expect(ring.take(got) && got == 7 && ring.take(got) && got == 8 && !ring.take(got), "order");

    pitlane::FillRing<int, 4> span;
    int* a = span.claim();
    *a = 1;
    expect(span.publish(), "span one");
    int* b = span.claim();
    *b = 2;
    expect(span.publish(), "span two");
    int* c = span.claim();
    *c = 3;
    expect(span.publish(), "span three");
    int out[4]{};
    expect(span.take_span(out, 1) == 1 && out[0] == 1, "one of the span");
    expect(span.take_span(out, 4) == 2 && out[0] == 2 && out[1] == 3, "rest of the span");
    expect(span.take_span(out, 4) == 0 && span.take_span(nullptr, 1) == 0, "span empty");
}

static void check_chunk() {
    expect(pitlane::floor_chunk(-65, 64) == -128, "negative block");
    pitlane::ChunkMap<64, 2> book;
    expect(book.write(3, 4) && book.write(70, 8), "two blocks");
    int64_t qty = 0;
    expect(book.read(3, &qty) && qty == 4, "first tick");
    expect(!book.write(140, 1) && book.read(3, &qty) && qty == 4, "full directory keeps old data");

    pitlane::ChainNode pool[4]{};
    pitlane::ChainLevel level;
    pitlane::ChainLoc first{};
    pitlane::ChainLoc second{};
    expect(pitlane::chain_link(pool, 4, level, 0, 5, &first), "link first");
    expect(pitlane::chain_link(pool, 4, level, 1, 7, &second) && level.total == 12, "link second");
    expect(pitlane::chain_zero(pool, 4, level, first) && level.total == 7, "zero stays linked");
    expect(pitlane::chain_take(pool[0], 3) == 0, "zero quantity is skipped");
    expect(pitlane::chain_unlink(pool, 4, level, first), "unlink");
    expect(!pitlane::chain_unlink(pool, 4, level, first), "old handle");
}

static void check_mold() {
    pitlane::IdSlip ids;
    expect(ids.issue() == 1 && ids.undo() && ids.issue() == 1, "undo the unsent id");
    expect(ids.issue() == 2 && ids.undo() && ids.issue() == 2, "only the latest id is returned");
    ids.keep();
    expect(!ids.undo(), "a kept id stays issued");

    uint8_t storage[32]{};
    const uint8_t prefix[] = {1, 2, 3, 4, 5, 6, 7, 8};
    pitlane::WireMold mold;
    expect(mold.seal(storage, 32, prefix, 8) && mold.put_i64(0, 42), "patch one field");
    expect(storage[0] == 42 && storage[1] == 0, "little endian");

    uint8_t frame[24]{};
    frame[0] = 12;
    frame[2] = 7;
    frame[8] = 9;
    pitlane::BlockHead head{};
    const uint8_t* body = nullptr;
    expect(pitlane::read_block(frame, 24, 8, head, &body) && head.tmpl == 7 && body[0] == 9, "wider block");
    expect(!pitlane::read_block(frame, 24, 16, head, &body), "older reader rejects a short block");
    expect(pitlane::block_end(frame, 24) == frame + 20, "skip the extra bytes");
}

int main() {
    check_numeric();
    check_wide();
    check_text();
    check_frame();
    check_hex_mix_text();
    check_drill();
    check_stream();
    check_gate_pace();
    check_snap();
    check_sig_grace();
    check_send_join();
    check_stage_seq_fill();
    check_readers();
    check_seqpub();
    check_global_scale();
    check_simd();
    check_bound_spin_arena();
    check_touch();
    check_chunk();
    check_mold();
    if (g_fails) {
        std::fprintf(stderr, "%d util check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
