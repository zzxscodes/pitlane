#pragma once

#include <cstdint>
#include <type_traits>

// Integer prices and quantities. Time stamps are microseconds.
// A side value of 1 buys, 2 sells. Zero is unset.

namespace pitlane {

enum class Side : uint8_t { Buy = 1, Sell = 2 };

enum class Act : uint8_t { Enter = 1, Pull = 2 };

// How much of a crossing order may trade, and whether the rest stays.
enum class Style : uint8_t {
    Day = 1,        // trade better prices, rest the balance, fill the limit rung in full
    Passive = 2,    // expire if it would cross
    Immediate = 3,  // trade shown size, expire the balance
    All = 4         // trade only when shown size covers the whole order
};

enum class Phase : uint8_t {
    None = 0,
    Staged = 1,
    Working = 2,
    Partial = 3,
    PullWait = 4,
    Filled = 5,
    Cancelled = 6,
    Denied = 7,
    Expired = 8
};

enum class Why : uint16_t {
    None = 0,
    Qty = 1,
    Side = 2,
    Dup = 3,
    Stamp = 4,
    Mode = 5,
    Name = 6,
    Acct = 7,
    Band = 8,
    Wash = 9,
    Rate = 10,
    Notional = 11,
    Full = 12,
    Missing = 13,
    Book = 14,
    Closed = 15  // session closed / NotTrading role
};

struct Intent {
    uint64_t tag;
    uint32_t name;
    uint32_t acct;
    int64_t px;
    int64_t qty;
    int64_t stamp_us;
    uint8_t side;
    uint8_t act;
    uint8_t style;
    uint8_t pad0;
    int32_t lot;
    int32_t pad1;
};

struct Report {
    uint64_t tag;
    uint64_t contra;
    int64_t px;
    int64_t qty;
    int64_t left;
    int64_t stamp_us;
    uint16_t why;
    uint8_t phase;
    uint8_t side;
    uint32_t name;
};

struct Quote {
    uint32_t name;
    uint32_t pad;
    int64_t exch_us;
    int64_t local_us;
    int64_t bid_px;
    int64_t ask_px;
    int64_t last_px;
    int64_t bid_qty;
    int64_t ask_qty;
    int64_t last_qty;
};

static_assert(std::is_trivially_copyable<Intent>::value, "Intent");
static_assert(std::is_trivially_copyable<Report>::value, "Report");
static_assert(std::is_trivially_copyable<Quote>::value, "Quote");

inline bool buy_side(uint8_t side) { return side == static_cast<uint8_t>(Side::Buy); }
inline bool sell_side(uint8_t side) { return side == static_cast<uint8_t>(Side::Sell); }
inline bool known_side(uint8_t side) { return buy_side(side) || sell_side(side); }

}  // namespace pitlane
