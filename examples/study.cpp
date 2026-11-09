#include "pitlane/pitlane.hpp"

#include <cstdio>

// Features, a caller decision, a markout, and a corporate-action factor.
// The decision function belongs to this example. The library does not choose the side.

namespace {

int fail(const char* what) {
    std::fprintf(stderr, "study: %s\n", what);
    return 1;
}

bool choose(const pitlane::Feat& feat, const pitlane::BookTop& top, const int64_t*, int, pitlane::Idea& idea, void*) {
    if (feat.spread != 1) return false;
    idea.px = top.ask_px;
    idea.qty = 1;
    idea.side = 1;
    return true;
}

}  // namespace

int main() {
    pitlane::BookTop top{};
    top.bid_px = 100;
    top.ask_px = 101;
    top.bid_qty = 4;
    top.ask_qty = 2;
    pitlane::Feat feat{};
    if (!pitlane::feat_of(top, feat) || feat.spread != 1) return fail("feat");

    pitlane::Horizon horizon{};
    horizon.entry = 101;
    horizon.side = 1;
    horizon.later[0] = 105;
    int64_t ticks[4]{};
    if (!pitlane::horizon_ticks(horizon, 1, ticks) || ticks[0] != 4) return fail("mark");
    if (pitlane::right_value(1, 105, 100) != 5) return fail("right");

    pitlane::RelBook rel;
    if (!rel.bind(2, 1) || rel.underlying(2) != 1) return fail("rel");
    pitlane::FactorBoard factors;
    int64_t value = 0;
    if (!factors.put(2, 0, 7) || !factors.get(2, 0, value) || value != 7) return fail("factor");

    const int rows[] = {100, 101};
    pitlane::Replay<int> replay;
    replay.data = rows;
    replay.n = 2;
    const int* item = nullptr;
    if (!replay.take(item) || *item != 100) return fail("replay");

    const pitlane::CorpAct acts[] = {{20240102, 1, 2, 100, 0, 0}};
    pitlane::PxMul factor{};
    int64_t adjusted = 0;
    if (!pitlane::mul_fold(acts, 1, 20240102, factor) || !pitlane::scale_px(50, factor, adjusted) || adjusted != 100)
        return fail("corp");

    pitlane::QuoteFold<8> book;
    const pitlane::FoldEdit edits[] = {
        {pitlane::FoldOp::Set, 1, 100, 4},
        {pitlane::FoldOp::Set, 2, 101, 2},
    };
    const int64_t factor_row[] = {7};
    pitlane::GateAsk gate{};
    gate.mode = pitlane::PxMode::Limit;
    gate.off = pitlane::Off::Open;
    gate.last = 100;
    gate.dev_den = 100;
    gate.dev_cap = 50;
    pitlane::Sheet sheet{};
    const pitlane::Cause cause =
        pitlane::study_step(book, edits, 2, factor_row, 1, choose, nullptr, &gate, &sheet, 8, 1000, feat, top);
    if (cause != pitlane::Cause::Ok || sheet.phase != pitlane::Phase::Staged || sheet.px != 101) return fail("step");

    std::printf("study spread=%lld mark=%lld adjusted=%lld phase=%s\n", static_cast<long long>(feat.spread),
                static_cast<long long>(ticks[0]), static_cast<long long>(adjusted),
                pitlane::phase_text(sheet.phase));
    return 0;
}
