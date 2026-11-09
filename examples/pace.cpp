#include "pitlane/pitlane.hpp"

#include <cstdio>
#include <cstring>

// Bytes, pacing, a published snapshot, and the fixed tables around a measurement.

int fail(const char* what) {
    std::fprintf(stderr, "pace: %s\n", what);
    return 1;
}

int main() {
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    if (pitlane::digest_of(pitlane::FrameDigest::Crc32, digits, 9) != 0xCBF43926u) return fail("crc");

    pitlane::SeqGate<4> gate;
    uint64_t seqs[4]{};
    if (!gate.start(1) || gate.live(1, seqs, 4) != 1 || seqs[0] != 1) return fail("seq");

    pitlane::TokenPot pot;
    if (!pitlane::token_open(pot, 10, 1, 1, 0) || !pitlane::token_take(pot, 0, 10) || pitlane::token_take(pot, 0, 1))
        return fail("token");

    pitlane::SignSlip slip;
    slip.need = 1;
    if (!pitlane::sign_pass(slip) || !pitlane::sign_ready(slip)) return fail("sign");

    pitlane::OpenQuota quota{10, 2};
    pitlane::OpenHold hold{};
    if (!pitlane::hold_reserve(quota, hold, true, 4) || quota.lots != 6) return fail("hold");

    pitlane::AckBook<2> acks;
    pitlane::VenueKey key{};
    if (!pitlane::key_num(key, 9, "ctp") || !acks.put(key, 11, 0)) return fail("ack");

    struct Sample {
        int v{0};
    };
    pitlane::SnapPub<Sample, 2> snap;
    Sample* slot = snap.begin_write();
    if (!slot) return fail("snap");
    slot->v = 3;
    if (!snap.publish()) return fail("publish");
    const Sample* held = snap.pin();
    if (!held || held->v != 3) return fail("pin");
    snap.unpin();

    pitlane::Grace<1> grace;
    grace.online(0);
    const uint64_t token = grace.bump();
    grace.quiesce(0);
    if (!grace.elapsed(token)) return fail("grace");

    pitlane::StageRing<uint64_t, 4> stages;
    uint32_t at = 0;
    uint32_t got_seq = 0;
    uint64_t body = 0;
    if (!stages.acquire(at) || !stages.finish(at, 0, 10) || !stages.take(0, got_seq, body) || body != 10) return fail("stage");

    pitlane::SeqWin<8> win;
    if (win.push(10, 1) != pitlane::SeqPut::In) return fail("window");
    uint64_t pulled[2]{};
    if (win.pull(pulled, 2) != 1 || pulled[0] != 10) return fail("pull");

    const uint8_t raw[] = {0x0a, 0xb0, 0x03};
    char hex[16]{};
    if (pitlane::hex_encode(raw, 3, hex, 16) != 6 || std::strcmp(hex, "0ab003") != 0) return fail("hex");

    pitlane::SampleBox<8> box;
    int64_t mean = 0;
    if (!box.add(1) || !box.add(3) || !box.mean(mean) || mean != 2) return fail("sample");

    pitlane::StepClock clock;
    if (!clock.jump(10) || !clock.step(5) || clock.now != 15) return fail("clock");

    pitlane::CycleProbe probe;
    probe.arm(0);
    uint64_t cycles = 0;
    if (!probe.begin() || probe.end(cycles) != 1) return fail("probe");

    std::printf("pace crc=%08x lots=%lld mean=%lld cycles=%llu\n", 0xCBF43926u, static_cast<long long>(quota.lots),
                static_cast<long long>(mean), static_cast<unsigned long long>(cycles));
    return 0;
}
