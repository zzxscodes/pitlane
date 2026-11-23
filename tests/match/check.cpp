#include "pitlane/match/kernel.hpp"

#include <cstdint>
#include <cstdio>

static int g_fails = 0;

static void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

static pitlane::MkIn mk(uint64_t tag, uint8_t side, int64_t px, int64_t qty, uint8_t style = 1) {
    pitlane::MkIn in{};
    in.tag = tag;
    in.acct = side == 1 ? 1u : 2u;
    in.px = px;
    in.qty = qty;
    in.side = side;
    in.style = style;
    return in;
}

static void check_continuous() {
    pitlane::MatchingKernel k;
    k.set_phase(pitlane::kMkContinuous);
    pitlane::MkFill fills[8]{};
    pitlane::MkAck ack{};

    // 卖 100 x 3 挂单，买 101 x 5 主动成交 3，剩 2 挂在 101。
    expect(k.submit(mk(1, 2, 100, 3), fills, 8, ack) == 0, "rest ask");
    expect(k.resting() == 1 && ack.why == pitlane::kMkOk, "ask resting");
    expect(k.submit(mk(2, 1, 101, 5), fills, 8, ack) == 1, "cross fill");
    expect(fills[0].maker == 1 && fills[0].qty == 3 && fills[0].px == 100, "fill price qty");
    expect(ack.left == 2 && k.resting() == 1, "remainder rests");
    int64_t bid = 0, ask = 0;
    (void)k.best(&bid, &ask);
    expect(bid == 101 && ask == 0, "best after cross");

    // 同价时间优先：101 上已有的 2 手先成交。
    k.reset();
    k.set_phase(pitlane::kMkContinuous);
    expect(k.submit(mk(10, 2, 200, 2), fills, 8, ack) == 0, "a");
    expect(k.submit(mk(11, 2, 200, 4), fills, 8, ack) == 0, "b");
    expect(k.queue_ahead(11) == 2, "queue ahead");
    expect(k.submit(mk(12, 1, 200, 5), fills, 8, ack) == 2, "two makers");
    expect(fills[0].maker == 10 && fills[0].qty == 2, "first maker");
    expect(fills[1].maker == 11 && fills[1].qty == 3, "second maker");
    expect(k.leaves(11) == 1, "maker leaves");

    // IOC 剩余撤销；FOK 不足整单拒绝；Passive 交叉拒绝。
    k.reset();
    k.set_phase(pitlane::kMkContinuous);
    expect(k.submit(mk(20, 2, 300, 3), fills, 8, ack) == 0, "rest 300");
    expect(k.submit(mk(21, 1, 300, 10, 3), fills, 8, ack) == 1, "ioc fill");
    expect(ack.left == 7 && k.resting() == 0, "ioc no rest");
    expect(k.submit(mk(22, 2, 301, 10), fills, 8, ack) == 0, "rest 301");
    expect(k.submit(mk(23, 1, 302, 20, 4), fills, 8, ack) == 0 && ack.why == pitlane::kMkWhyQty, "fok reject");
    expect(k.submit(mk(24, 1, 302, 10, 2), fills, 8, ack) == 0 && ack.why == pitlane::kMkWhyPrice, "passive reject");

    // 撤单与改单。
    expect(k.cancel(22), "cancel");
    expect(!k.cancel(22), "cancel twice");
    expect(k.resting() == 0, "cancel empties");
    expect(k.submit(mk(30, 2, 305, 6), fills, 8, ack) == 0, "amend rest");
    pitlane::MkIn am = mk(30, 2, 304, 8);
    expect(k.amend(am, fills, 8, ack) == 0 && k.leaves(30) == 8 && k.queue_ahead(30) == 0, "amend reprices");

    // 涨跌停。
    k.reset();
    k.set_phase(pitlane::kMkContinuous);
    k.set_limits(310, 320);
    expect(k.submit(mk(40, 1, 309, 1), fills, 8, ack) == 0 && ack.why == pitlane::kMkWhyBand, "band down");
    expect(k.submit(mk(41, 2, 321, 1), fills, 8, ack) == 0 && ack.why == pitlane::kMkWhyBand, "band up");
    // 改单被拒时原单保留，队尾不再重排。
    expect(k.submit(mk(48, 2, 315, 5), fills, 8, ack) == 0 && k.leaves(48) == 5, "amend keeper rest");
    expect(k.amend(mk(48, 2, 330, 5), fills, 8, ack) == 0 && ack.why == pitlane::kMkWhyBand && k.leaves(48) == 5,
           "amend band reject keeps order");
    k.set_phase(pitlane::kMkHalted);
    expect(k.amend(mk(48, 2, 315, 6), fills, 8, ack) == 0 && ack.why == pitlane::kMkWhyHalted && k.leaves(48) == 5,
           "amend halted keeps order");
    k.set_phase(pitlane::kMkContinuous);
    expect(k.amend(mk(48, 2, 316, 6), fills, 8, ack) == 0 && k.leaves(48) == 6, "amend after reject applies");
    expect(k.cancel(48), "amend keeper done");
    expect(k.submit(mk(42, 1, 320, 1), fills, 8, ack) == 0 && ack.why == pitlane::kMkOk, "band edge");

    // 停牌期间只允许撤单。
    k.set_limits(0, 0);
    k.halt(true);
    expect(k.submit(mk(43, 1, 315, 1), fills, 8, ack) == 0 && ack.why == pitlane::kMkWhyHalted, "halted submit");
    expect(k.cancel(42), "halted cancel");
    k.halt(false);
    expect(k.phase() == pitlane::kMkContinuous, "resume");
}

static void check_queue_print() {
    pitlane::MatchingKernel k;
    k.set_phase(pitlane::kMkContinuous);
    pitlane::MkFill fills[8]{};
    pitlane::MkAck ack{};
    expect(k.submit(mk(1, 1, 100, 5), fills, 8, ack) == 0, "q rest a");
    expect(k.submit(mk(2, 1, 100, 10), fills, 8, ack) == 0, "q rest b");
    expect(k.queue_ahead(2) == 5, "q ahead b");

    expect(k.on_print(100, 8, 2, fills, 8) == 1, "print fill a");
    expect(fills[0].maker == 1 && fills[0].qty == 5, "queue fill a");
    expect(k.queue_ahead(2) == 2, "queue eaten");
    expect(k.leaves(2) == 10, "b untouched");

    expect(k.on_print(100, 5, 2, fills, 8) == 1, "print fills b");
    expect(fills[0].maker == 2 && fills[0].qty == 3, "queue fill b");
    expect(k.leaves(2) == 7, "b leaves");

    // 主动买打印不消耗买方排队。
    expect(k.on_print(100, 100, 1, fills, 8) == 0, "buy print ignores bids");
}

static void check_auction() {
    pitlane::MatchingKernel k;
    k.set_phase(pitlane::kMkCall);
    pitlane::MkFill fills[8]{};
    pitlane::MkAck ack{};
    expect(k.submit(mk(1, 1, 100, 10), fills, 8, ack) == 0, "call bid 100");
    expect(k.submit(mk(2, 1, 99, 10), fills, 8, ack) == 0, "call bid 99");
    expect(k.submit(mk(3, 2, 101, 5), fills, 8, ack) == 0, "call ask 101");
    expect(k.submit(mk(4, 2, 100, 10), fills, 8, ack) == 0, "call ask 100");
    expect(k.resting() == 4, "call all rest");

    int64_t px = 0;
    const int64_t vol = k.uncross(100, 0, fills, 8, &px);
    expect(vol == 10, "auction volume");
    expect(px == 100, "auction price");
    expect(fills[0].px == 100 && fills[0].qty == 10, "auction fill");
    expect(k.leaves(2) == 10 && k.leaves(3) == 5, "auction leftovers");
    expect(!k.has(1) && !k.has(4), "auction filled");
    expect(k.stats().auction_fills == 1 && k.stats().matched_qty == 10, "auction stats");

    // 无交叉时无成交。
    pitlane::MatchingKernel k2;
    k2.set_phase(pitlane::kMkCall);
    expect(k2.submit(mk(5, 1, 90, 1), fills, 8, ack) == 0, "k2 bid");
    expect(k2.submit(mk(6, 2, 95, 1), fills, 8, ack) == 0, "k2 ask");
    expect(k2.uncross(92, 1, fills, 8, &px) == 0 && px == 0, "no auction cross");
}

static void check_self_trade() {
    pitlane::MatchingKernel k;
    k.set_phase(pitlane::kMkContinuous);
    pitlane::MkFill fills[8]{};
    pitlane::MkAck ack{};
    pitlane::MkIn a = mk(1, 2, 100, 5);
    a.acct = 7;
    expect(k.submit(a, fills, 8, ack) == 0, "self rest");
    pitlane::MkIn b = mk(2, 1, 100, 5);
    b.acct = 7;
    expect(k.submit(b, fills, 8, ack) == 0 && k.leaves(1) == 5, "self blocked");
    b.acct = 8;
    b.tag = 3;
    expect(k.submit(b, fills, 8, ack) == 1, "other acct crosses");
}

static void check_limit_after_level() {
    pitlane::MatchingKernel k;
    k.set_phase(pitlane::kMkContinuous);
    pitlane::MkFill fills[8]{};
    pitlane::MkAck ack{};
    expect(k.submit(mk(201, 2, 100, 1), fills, 8, ack) == 0, "level rest 100");
    expect(k.submit(mk(202, 2, 101, 1), fills, 8, ack) == 0, "level rest 101");

    // 买限价 100：吃完 100 档后不得越过限价去吃 101。
    expect(k.submit(mk(203, 1, 100, 2), fills, 8, ack) == 1, "limit stops at level");
    expect(fills[0].px == 100 && fills[0].qty == 1, "limit fill only 100");
    expect(k.leaves(202) == 1, "worse level untouched");
    expect(k.has(203) && k.leaves(203) == 1, "remainder rests at limit");
}

static void check_best_five_levels() {
    pitlane::MatchingKernel k;
    k.set_phase(pitlane::kMkContinuous);
    pitlane::MkFill fills[8]{};
    pitlane::MkAck ack{};
    for (int i = 0; i < 5; ++i) {
        expect(k.submit(mk(300 + i, 2, 100, 1), fills, 8, ack) == 0, "best five asks at 100");
    }
    expect(k.submit(mk(305, 2, 101, 1), fills, 8, ack) == 0, "best five ask at 101");

    // 第一档有 5 张单，仍应按价格档计数，继续吃第二档。
    const int n = k.submit(mk(306, 1, 101, 10, 5), fills, 8, ack);
    expect(n == 6, "best five counts levels not orders");
    expect(fills[5].px == 101, "second level reached");
    expect(ack.left == 4 && k.resting() == 0, "best five drops remainder");
}

static void check_auction_self_trade() {
    pitlane::MatchingKernel k;
    k.set_phase(pitlane::kMkCall);
    pitlane::MkFill fills[8]{};
    pitlane::MkAck ack{};
    pitlane::MkIn bid = mk(401, 1, 100, 10);
    bid.acct = 7;
    pitlane::MkIn ask = mk(402, 2, 100, 10);
    ask.acct = 7;
    expect(k.submit(bid, fills, 8, ack) == 0, "self bid rests");
    expect(k.submit(ask, fills, 8, ack) == 0, "self ask rests");

    int64_t px = 0;
    expect(k.uncross(100, 0, fills, 8, &px) == 0, "self trade not matched");
    expect(px == 0, "no auction price");
    expect(k.leaves(401) == 10 && k.leaves(402) == 10, "both sides kept");
}

static void check_print_phase_and_side() {
    pitlane::MatchingKernel k;
    k.set_phase(pitlane::kMkContinuous);
    pitlane::MkFill fills[8]{};
    pitlane::MkAck ack{};
    expect(k.submit(mk(501, 1, 100, 5), fills, 8, ack) == 0, "print bid rests");

    // 卖主动打印应记 taker_side=2。
    expect(k.on_print(100, 5, 2, fills, 8) == 1, "print fills bid");
    expect(fills[0].taker_side == 2, "print taker side");

    expect(k.submit(mk(502, 1, 100, 5), fills, 8, ack) == 0, "halted print bid rests");
    k.halt(true);
    expect(k.on_print(100, 5, 2, fills, 8) == 0, "no print fill while halted");
    expect(k.leaves(502) == 5, "halted print leaves order");
}

static void check_best_five() {
    pitlane::MatchingKernel k;
    k.set_phase(pitlane::kMkContinuous);
    pitlane::MkFill fills[8]{};
    pitlane::MkAck ack{};
    for (int i = 0; i < 6; ++i) {
        expect(k.submit(mk(100 + i, 2, 100 + i, 1), fills, 8, ack) == 0, "best five rest");
    }
    // 六档卖盘中，最优五档吃满 5 手，剩 1 手不成交也不挂单。
    const int n = k.submit(mk(200, 1, 105, 6, 5), fills, 8, ack);
    expect(n == 5, "best five fills five levels");
    expect(fills[4].px == 104, "fifth level only");
    expect(ack.left == 1 && k.resting() == 1, "best five drops the rest");
    expect(k.leaves(105) == 1, "sixth level untouched");

    // 展示量足够时，最优五档全部成交。
    expect(k.submit(mk(201, 1, 105, 1, 5), fills, 8, ack) == 1, "sixth level crossed");
    expect(ack.left == 0, "no remainder");
}

int main() {
    check_continuous();
    check_queue_print();
    check_auction();
    check_self_trade();
    check_limit_after_level();
    check_best_five_levels();
    check_auction_self_trade();
    check_print_phase_and_side();
    check_best_five();
    if (g_fails) {
        std::fprintf(stderr, "%d match checks failed\n", g_fails);
        return 1;
    }
    std::printf("match ok\n");
    return 0;
}
