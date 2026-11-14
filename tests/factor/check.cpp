#include "pitlane/study/factor.hpp"
#include "pitlane/study/factor_eval.hpp"
#include "pitlane/study/pipeline.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_fails = 0;

static void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

static constexpr uint8_t kReturn = static_cast<uint8_t>(pitlane::FactorKind::Return);
static constexpr uint8_t kMomentum = static_cast<uint8_t>(pitlane::FactorKind::Momentum);
static constexpr uint8_t kVolatility = static_cast<uint8_t>(pitlane::FactorKind::Volatility);
static constexpr uint8_t kZScore = static_cast<uint8_t>(pitlane::FactorKind::ZScore);
static constexpr uint8_t kEma = static_cast<uint8_t>(pitlane::FactorKind::Ema);
static constexpr uint8_t kRsi = static_cast<uint8_t>(pitlane::FactorKind::Rsi);
static constexpr uint8_t kOfi = static_cast<uint8_t>(pitlane::FactorKind::Ofi);
static constexpr uint8_t kImbalance = static_cast<uint8_t>(pitlane::FactorKind::Imbalance);

// Same specs, samples, and quotes as the hand-checked engine above.
static void load_engine(pitlane::FactorEngine<8, 256>& e) {
    e.add({1, kReturn, 2, 0});
    e.add({2, kMomentum, 2, 0});
    e.add({3, kVolatility, 4, 0});
    e.add({4, kZScore, 4, 0});
    e.add({5, kEma, 4, 0});
    e.add({6, kRsi, 4, 0});
    e.add({7, kOfi, 2, 0});
    e.add({8, kImbalance, 1, 0});
    e.on_sample(100, 1000);
    e.on_sample(101, 2000);
    e.on_sample(102, 3000);
    e.on_sample(101, 4000);
    e.on_sample(100, 5000);
    e.on_quote(99, 101, 5, 1, 6000);
    e.on_quote(99, 101, 8, 1, 7000);
}

static void check_engine() {
    pitlane::FactorEngine<8, 256> eng;
    expect(eng.add({1, kReturn, 2, 0}), "add return");
    expect(eng.add({2, kMomentum, 2, 0}), "add momentum");
    expect(eng.add({3, kVolatility, 4, 0}), "add volatility");
    expect(eng.add({4, kZScore, 4, 0}), "add zscore");
    expect(eng.add({5, kEma, 4, 0}), "add ema");
    expect(eng.add({6, kRsi, 4, 0}), "add rsi");
    expect(eng.add({7, kOfi, 2, 0}), "add ofi");
    expect(eng.add({8, kImbalance, 1, 0}), "add imbalance");
    expect(eng.count() == 8, "spec count");
    expect(!eng.add({9, kReturn, 2, 0}), "names full");

    pitlane::FactorEngine<2, 16> bad;
    expect(!bad.add({1, 0, 2, 0}), "bad kind");
    expect(!bad.add({1, 9, 2, 0}), "bad kind high");
    expect(!bad.add({1, kReturn, 0, 0}), "zero window");
    expect(!bad.add({1, kReturn, 17, 0}), "window beyond depth");
    expect(bad.add({1, kReturn, 16, 0}), "window at depth");
    expect(bad.add({2, kReturn, 2, 0}), "second spec");
    expect(!bad.add({3, kReturn, 2, 0}), "table full");

    pitlane::FactorValue v{};
    v.ready = 9;
    expect(!eng.value(0, v) && v.ready == 0, "no samples");
    expect(!eng.value(-1, v), "bad slot low");
    expect(!eng.value(8, v), "bad slot high");

    expect(eng.on_sample(100, 1000), "sample 100");
    expect(!eng.value(0, v) && v.ready == 0, "return gate one sample");
    expect(!eng.value(2, v), "vol gate one sample");
    expect(eng.samples() == 1, "samples one");

    expect(eng.on_sample(101, 2000), "sample 101");
    expect(eng.value(0, v) && v.ready == 1 && v.value == 10000 && v.stamp_us == 2000, "return up");
    expect(eng.value(1, v) && v.ready == 1 && v.value == 10000, "momentum up");

    expect(eng.on_sample(102, 3000), "sample 102");
    expect(eng.value(2, v) && v.ready == 1 && v.value > 0, "vol ready positive");
    expect(eng.value(3, v) && v.ready == 1 && v.value == 0, "zs integer variance zero");
    expect(eng.value(4, v) && v.ready == 1, "ema ready");
    expect(eng.value(5, v) && v.ready == 1 && v.value >= 0 && v.value <= 100000000, "rsi bounds");

    expect(eng.on_sample(101, 4000), "sample 101b");
    expect(eng.on_sample(100, 5000), "sample 100b");
    expect(eng.value(0, v) && v.value == -9900, "return down");
    expect(eng.value(1, v) && v.value == -9900, "momentum down");
    expect(eng.value(4, v) && v.stamp_us == 5000, "ema stamp");

    expect(!eng.value(7, v), "imbalance gate");
    expect(!eng.on_quote(102, 101, 1, 1, 1), "reject crossed quote");
    expect(!eng.on_quote(-1, 101, 1, 1, 1), "reject negative bid");
    expect(eng.on_quote(99, 101, 5, 1, 6000), "quote one");
    expect(eng.quotes() == 1, "quotes one");
    expect(eng.value(7, v) && v.ready == 1 && v.value > 0 && v.stamp_us == 6000, "imbalance bid heavy");
    expect(!eng.value(6, v), "ofi gate two quotes");

    expect(eng.on_quote(99, 101, 8, 1, 7000), "quote two");
    expect(eng.value(6, v) && v.ready == 1 && v.value != 0 && v.stamp_us == 7000, "ofi nonzero");
    expect(eng.value(7, v) && v.value > 0, "imbalance still bid heavy");

    pitlane::FactorEngine<8, 256> twin;
    load_engine(twin);
    for (int slot = 0; slot < 8; ++slot) {
        pitlane::FactorValue a{};
        pitlane::FactorValue b{};
        expect(twin.value(slot, a) && eng.value(slot, b), "determinism ready");
        expect(a.value == b.value && a.stamp_us == b.stamp_us && a.ready == b.ready, "determinism same");
    }
}

static void check_eval() {
    pitlane::FactorEval<64> ev;
    const int64_t f[6] = {-3, -2, -1, 1, 2, 3};
    const int64_t r[6] = {-3, -2, -1, 1, 2, 3};
    for (int i = 0; i < 6; ++i) expect(ev.observe(f[i], r[i]), "eval observe");
    expect(ev.size() == 6, "eval size");

    pitlane::FactorEval<64>::Eval out{};
    expect(ev.result(out), "eval result");
    expect(out.n == 6, "eval n");
    expect(out.ic_ppm > 999000 && out.ic_ppm <= 1000000, "ic near one");
    expect(out.rank_ic_ppm > 999000 && out.rank_ic_ppm <= 1000000, "rank ic near one");
    expect(out.hit_rate_ppm == 1000000, "hit rate one");
    expect(out.top_quintile_ppm > out.bottom_quintile_ppm, "quintile top above bottom");
    expect(out.mean_factor == 0 && out.mean_return == 0, "eval means");

    pitlane::FactorEval<64> rev;
    const int64_t rr[6] = {3, 2, 1, -1, -2, -3};
    for (int i = 0; i < 6; ++i) rev.observe(f[i], rr[i]);
    pitlane::FactorEval<64>::Eval rout{};
    expect(rev.result(rout), "reverse result");
    expect(rout.ic_ppm < -999000 && rout.ic_ppm >= -1000000, "ic near minus one");
    expect(rout.rank_ic_ppm < -999000 && rout.rank_ic_ppm >= -1000000, "rank ic minus one");
    expect(rout.hit_rate_ppm == 0, "hit rate zero");
    expect(rout.top_quintile_ppm < rout.bottom_quintile_ppm, "reverse quintile");

    pitlane::FactorEval<8> flat;
    const int64_t zf[4] = {5, 5, 5, 5};
    const int64_t zr[4] = {1, 2, 3, 4};
    for (int i = 0; i < 4; ++i) flat.observe(zf[i], zr[i]);
    pitlane::FactorEval<8>::Eval zout{};
    expect(flat.result(zout) && zout.n == 4 && zout.ic_ppm == 0, "zero variance ic");

    pitlane::FactorEval<8> tiny;
    tiny.observe(1, 1);
    tiny.observe(2, 2);
    pitlane::FactorEval<8>::Eval tout{};
    tout.n = -1;
    expect(!tiny.result(tout) && tout.n == -1, "too few leaves out");

    for (int i = 0; i < 4; ++i) expect(flat.observe(1, 1), "fill eval");
    expect(!flat.observe(1, 1), "eval full");
}

static void check_pipeline() {
    pitlane::FactorPipeline<4, 64, 64> pipe;
    expect(pipe.add({1, kReturn, 2, 0}), "pipe add");
    expect(pipe.set_stage(pitlane::PipeStage::Research), "research stage");
    expect(pipe.stage() == pitlane::PipeStage::Research, "stage get");
    expect(!pipe.set_stage(static_cast<pitlane::PipeStage>(0)), "stage zero rejected");
    expect(!pipe.set_stage(static_cast<pitlane::PipeStage>(5)), "stage five rejected");

    expect(pipe.on_sample(100, 1000, 0), "pipe sample 100");
    expect(pipe.eval_size() == 0, "eval empty before two samples");
    expect(pipe.on_sample(101, 2000, 7), "pipe sample 101");
    expect(pipe.eval_size() == 1, "eval grows one");
    expect(pipe.on_quote(99, 101, 3, 3, 2100), "pipe quote");
    expect(pipe.on_sample(102, 3000, 5), "pipe sample 102");
    expect(pipe.on_sample(101, 4000, 2), "pipe sample 101b");
    expect(pipe.eval_size() == 3, "eval grows three");

    pitlane::FactorEval<64>::Eval out{};
    expect(pipe.eval(out) && out.n == 3, "pipe eval forwards");

    expect(pipe.set_stage(pitlane::PipeStage::Live), "live stage");
    expect(pipe.on_sample(103, 5000, 9), "live sample");
    expect(pipe.eval_size() == 3, "live skips eval");

    pitlane::SigNote note{};
    expect(pipe.signal(0, 1, note, 42, "ret", 9000), "signal emits");
    expect(note.name == 42 && note.stamp_us == 9000 && note.last == 1 && note.seq == 1, "signal fields");
    expect(note.value > 0 && std::strcmp(note.topic, "ret") == 0, "signal value and topic");
    expect(!pipe.signal(0, note.value + 1, note, 42, "ret", 9001), "signal below threshold");
    expect(!pipe.signal(1, 0, note, 42, "ret", 9002), "signal bad slot");

    pitlane::FactorPipeline<2, 32, 16> fresh;
    expect(fresh.add({1, kReturn, 2, 0}), "fresh add");
    pitlane::SigNote fresh_note{};
    expect(!fresh.signal(0, 0, fresh_note, 1, "x", 1), "signal not ready");
}

int main() {
    check_engine();
    check_eval();
    check_pipeline();
    if (g_fails) {
        std::fprintf(stderr, "%d factor check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
