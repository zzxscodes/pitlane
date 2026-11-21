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
    Immediate = 3,  // trade shown size, expire the balance (FAK)
    All = 4,        // trade only when shown size covers the whole order (FOK)
    BestFive = 5    // trade up to five levels of shown size, expire the balance
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
    Closed = 15,  // session closed
    Biz = 16      // business kind the counter cannot carry
};

// Order offset, numbered exactly like CTP THOST_FTDC_OF_* ('0'..'6').
// Futures venues separate open from close, and SHFE/INE also separate today's
// position from yesterday's. ForceClose, ForceOff, and LocalForceClose are the
// counter's own close codes: they act as a close but keep their own value so
// the adapter can forward it unchanged. Intent::force_reason carries the
// separate THOST_FTDC_FCC_* reason.
enum class Off : int8_t {
    Open = 0,             // '0'
    Close = 1,            // '1'
    ForceClose = 2,       // '2' 强平
    CloseToday = 3,       // '3'
    CloseYday = 4,        // '4'
    ForceOff = 5,         // '5' 强减
    LocalForceClose = 6   // '6' 本地强平
};

inline bool off_known(Off off) noexcept { return off >= Off::Open && off <= Off::LocalForceClose; }

inline bool force_off(Off off) noexcept {
    return off == Off::ForceClose || off == Off::ForceOff || off == Off::LocalForceClose;
}

inline Off base_off(Off off) noexcept { return force_off(off) ? Off::Close : off; }

// Counter hedge flag. Speculation is the zero value so an Intent that leaves
// the field untouched keeps the old speculation-only behaviour. CTP has no
// covered-write flag, so Covered maps to THOST_FTDC_HF_Hedge.
enum class Hedge : uint8_t { Speculation = 0, Arbitrage = 1, Hedge = 2, Covered = 3 };

// Business kind on top of the buy/sell direction. Trade is the zero value so
// untouched Intents stay ordinary orders. The directional kinds still carry a
// side; the others are local postings that no futures counter accepts.
enum class Biz : uint8_t {
    Trade = 0,
    MarginBuy = 1,
    ShortSell = 2,
    BuyToCover = 3,
    SellToRepay = 4,
    Subscribe = 5,
    Redeem = 6,
    CashRepay = 7,
    ReturnStock = 8,
    OddLotTransfer = 9,
    CollateralIn = 10,
    CollateralOut = 11,
    Lock = 12,
    Unlock = 13,
    SplitOrder = 14,
    MergeOrder = 15,
    None = 255
};

inline bool biz_known(Biz biz) noexcept { return biz <= Biz::MergeOrder; }
inline bool biz_directional(Biz biz) noexcept { return biz >= Biz::Trade && biz <= Biz::Redeem; }

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
    Off off;
    int32_t lot;
    uint8_t hedge;
    uint8_t biz;         // Biz; 0 is an ordinary trade
    int8_t px_mode;      // PxMode; 0 limit, the other modes ignore px
    uint8_t force_reason;  // CTP THOST_FTDC_FCC_* value; 0 means not a forced close
    uint8_t pad1[4];
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
    uint8_t cause;  // Intent::force_reason echoed back; 0 means not a forced close
    uint8_t biz;    // Intent::biz echoed back; 0 means an ordinary trade
    uint8_t pad3[2];
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
static_assert(sizeof(Intent) == 56, "Intent stays one compact frame");
static_assert(std::is_trivially_copyable<Report>::value, "Report");
static_assert(sizeof(Report) == 64, "Report stays one cache line");
static_assert(std::is_trivially_copyable<Quote>::value, "Quote");

inline bool buy_side(uint8_t side) { return side == static_cast<uint8_t>(Side::Buy); }
inline bool sell_side(uint8_t side) { return side == static_cast<uint8_t>(Side::Sell); }
inline bool known_side(uint8_t side) { return buy_side(side) || sell_side(side); }

}  // namespace pitlane
