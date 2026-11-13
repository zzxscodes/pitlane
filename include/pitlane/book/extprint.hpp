#pragma once

// Print matching from paper matcher BackTestMatch.
// Prices are integers. A same-direction print still counts; the fill price differs.

#include "pitlane/book/arrive.hpp"

#include <cstdint>

namespace pitlane {

struct ExtPrint {
    uint64_t seq{0};
    uint64_t bid_no{0};
    uint64_t ask_no{0};
    int64_t px{0};
    int64_t qty{0};
};

enum class MatchPath : uint8_t {
    None = 0,
    ActiveBid = 1,
    ActiveAsk = 2,
    PassiveBid = 3,
    PassiveAsk = 4
};

struct PathFill {
    MatchPath path{MatchPath::None};
    int64_t px{0};
    int64_t qty{0};
    uint8_t same{0};
};

// Buy-initiated when the bid number is later. Sell-initiated when the ask number is later.
inline bool same_side_print(bool buy, uint64_t bid_no, uint64_t ask_no) noexcept {
    if (buy) return bid_no > ask_no;
    return ask_no > bid_no;
}

// Sequence must not precede the order. Rigid requires a price strictly through the slipped limit.
inline bool print_hits(bool buy, int64_t order_px, int64_t trade_px, int64_t slip, bool rigid, uint64_t order_seq,
                       uint64_t trade_seq) noexcept {
    if (trade_seq < order_seq || order_px <= 0 || trade_px <= 0 || slip < 0) return false;
    if (buy) {
        if (rigid) {
            if (order_px <= slip) return false;
            return trade_px < order_px - slip;
        }
        return trade_px <= order_px;
    }
    if (slip > INT64_MAX - order_px) return false;
    if (rigid) return trade_px > order_px + slip;
    return trade_px >= order_px;
}

// One resting order against one print (ExtMatchSingle). Same direction fills at the print.
// The other direction fills at the order price. Quantity is the smaller of the two.
inline PathFill ext_match_one(bool buy, int64_t order_px, int64_t leaves, int64_t slip, bool rigid, uint64_t order_seq,
                              const ExtPrint& print) noexcept {
    PathFill out;
    if (leaves <= 0 || print.qty <= 0) return out;
    if (!print_hits(buy, order_px, print.px, slip, rigid, order_seq, print.seq)) return out;
    const bool same = same_side_print(buy, print.bid_no, print.ask_no);
    out.same = same ? 1 : 0;
    out.path = buy ? MatchPath::PassiveBid : MatchPath::PassiveAsk;
    out.px = same ? print.px : order_px;
    out.qty = print.qty < leaves ? print.qty : leaves;
    return out;
}

// Active quote path (MatchBidOrders / MatchAskOrders). A cross takes shown size and stops.
// No cross leaves the order passive; the caller then uses prints.
inline PathFill match_quote(bool buy, int64_t px, int64_t leaves, const Rung* opp, int n) noexcept {
    PathFill out;
    if (leaves <= 0 || px <= 0 || !opp || n <= 0) return out;
    const bool cross = buy ? opp[0].px <= px : opp[0].px >= px;
    if (!cross) return out;
    const ArrivalHit hit = buy ? walk_buy(Style::Immediate, px, leaves, opp, n)
                               : walk_sell(Style::Immediate, px, leaves, opp, n);
    if (hit.filled <= 0) return out;
    out.path = buy ? MatchPath::ActiveBid : MatchPath::ActiveAsk;
    out.px = opp[0].px;
    out.qty = hit.filled;
    return out;
}

}  // namespace pitlane
