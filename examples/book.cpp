#include "pitlane/pitlane.hpp"

#include <cstdio>

// Quote book, price window, visible size, a bar, the session calendar, and a limit book.

int fail(const char* what) {
    std::fprintf(stderr, "book: %s\n", what);
    return 1;
}

int main() {
    pitlane::QuoteFold<8> fold;
    const pitlane::FoldEdit edits[] = {
        {pitlane::FoldOp::Set, 1, 100, 4},
        {pitlane::FoldOp::Set, 2, 101, 3},
    };
    if (!fold.apply(edits, 2)) return fail("fold");
    pitlane::FoldLevel bid{};
    pitlane::FoldLevel ask{};
    if (fold.copy(1, &bid, 1) != 1 || bid.px != 100) return fail("bid");
    if (fold.copy(2, &ask, 1) != 1 || ask.px != 101) return fail("ask");

    pitlane::PriceWindow<128> window;
    if (!window.reset(100, 1) || !window.set(1, 100, 4) || !window.set(2, 101, 3)) return fail("window");
    int64_t px = 0;
    int64_t qty = 0;
    if (!window.best(1, px, qty) || px != 100 || !window.best(2, px, qty) || px != 101) return fail("best");

    const int64_t prices[] = {100, 101};
    const int64_t sizes[] = {4, 3};
    if (pitlane::cover_qty(prices, sizes, 2, true, 101, 10) != 7) return fail("cover");

    pitlane::BarFold bars(60 * 1000000ll);
    pitlane::Bar closed{};
    if (!bars.on_tick(10 * 1000000ll, 100, 1, true, &closed)) return fail("bar");
    if (!bars.on_tick(70 * 1000000ll, 102, 2, true, &closed) || closed.close != 100) return fail("roll");

    pitlane::Span spans[8]{};
    const int nspan = pitlane::stock_spans(spans, 8);
    if (nspan <= 0 || !pitlane::minute_in_spans(10 * 60, spans, nspan)) return fail("spans");
    if (pitlane::next_session_day(20261107, nullptr) != 20261109) return fail("calendar");

    pitlane::LagTable lags{};
    lags.go_us[1] = 15;
    if (pitlane::table_lag_us(lags, 10 * 60, false) != 15) return fail("lag");

    pitlane::CrossBook book;
    int why = 0;
    pitlane::Deal deals[4]{};
    const pitlane::CrossIn rest{1, 1, 100, 5, 1, 1};
    const pitlane::CrossIn hit{2, 2, 100, 2, 2, 1};
    if (book.submit(rest, deals, 4, &why) != 0) return fail("rest");
    if (book.submit(hit, deals, 4, &why) != 1 || deals[0].qty != 2 || deals[0].px != 100) return fail("deal");

    pitlane::DepthBoard<4> depth;
    depth.insert_bid(0, 100, 4);
    pitlane::RankPx shown[4]{};
    depth.copy_bid(shown);
    if (shown[0].px != 100 || shown[0].qty != 4) return fail("depth");

    const pitlane::PxBand bands[] = {{0, 1000, 2}};
    if (pitlane::step_floor(bands, 1, 101) != 100 || pitlane::step_ceil(bands, 1, 101) != 102) return fail("step");

    const pitlane::CallIn calls[] = {{1, 100, 10, 1}, {2, 101, 10, 1}, {3, 99, 10, 2}, {4, 101, 10, 2}};
    pitlane::CallOut fills[4]{};
    int64_t matched = 0;
    if (pitlane::auction_clear(calls, 4, 100, 1, fills, 4, &matched) != 101 || matched != 10) return fail("auction");

    pitlane::Gap gap;
    pitlane::gap_join(gap, 50);
    if (pitlane::gap_print(gap, 54, 1, 4) != 4) return fail("gap");

    std::printf("book bid=%lld ask=%lld deal=%lld bar=%lld\n", static_cast<long long>(bid.px),
                static_cast<long long>(ask.px), static_cast<long long>(deals[0].qty),
                static_cast<long long>(closed.close));
    return 0;
}
