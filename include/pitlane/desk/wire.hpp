#pragma once

#include "pitlane/desk/ring.hpp"
#include "pitlane/order/ledger_view.hpp"

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace pitlane {

// Bounded records carried on the extra desk lanes.
// Prices and quantities are integers. Times are microseconds.

// One fixed price rung of an algo order. Quantities are integers.
struct AlgoRung {
    int64_t px{0};
    int64_t qty{0};  // <=0 takes the remaining parent size
};

struct AlgoJob {
    uint64_t tag{0};
    uint32_t name{0};
    uint32_t acct{0};
    int64_t px{0};
    int64_t qty{0};
    int64_t cont_begin_us{0};
    int64_t cont_end_us{0};
    int64_t call_begin_us{0};
    int64_t born_us{0};
    int64_t impact_bid{0};
    int64_t impact_ask{0};
    int32_t slip_ticks{0};
    int32_t call_slip{0};
    uint8_t side{0};
    uint8_t chase{0};       // 0 unset, 1 bid, 2 ask, 3 mid, 4 limit
    uint8_t call_chase{0};
    uint8_t roll_left{0};   // leftover call size continues into the continuous session
    uint8_t stop_up_sell{0};
    uint8_t stop_down_buy{0};
    uint8_t sparse{0};      // 0 front, 1 middle, 2 tail
    uint8_t phase{0};       // 1 running, 2 paused
    uint8_t saved_phase{0};
    char style_name[16]{};
    uint8_t rung_n{0};  // 0 uses px/qty; 1..8 walk rungs in order
    uint8_t pad[7]{};
    AlgoRung rungs[8]{};
};

// Counter order snapshot published by the sheet owner. Times are microseconds.
struct ExtOrder {
    uint64_t tag{0};
    uint64_t counter{0};
    uint32_t name{0};
    uint32_t acct{0};
    int64_t px{0};
    int64_t qty{0};
    int64_t filled{0};
    int64_t left{0};
    int64_t send_us{0};
    int64_t ack_us{0};
    int64_t done_us{0};
    uint8_t side{0};
    uint8_t phase{0};
    uint8_t cause{0};
    uint8_t off{0};
    uint8_t hedge{0};
    uint8_t pad[3]{};
};

struct ControlNote {
    uint64_t tag{0};
    uint32_t name{0};
    int64_t stamp_us{0};
    uint8_t kind{0};  // 1 stop, 2 reload, 3 pause, 4 resume, 5 new date, 6 new parameter
    char text[48]{};
};

// Reply for one control note. code 0 means the engine applied it; any other
// value is the refusal reason. tag and name echo the request.
struct ControlAck {
    uint64_t tag{0};
    uint32_t name{0};
    int32_t code{0};
    int64_t stamp_us{0};
    uint8_t kind{0};
    uint8_t applied{0};
    uint16_t pad{0};
    char text[48]{};
};

// Option exercise / abandon request. kind 1 exercises, kind 2 abandons.
struct ExecNote {
    uint64_t tag{0};
    uint32_t name{0};
    uint32_t acct{0};
    int64_t qty{0};
    int64_t stamp_us{0};
    uint8_t kind{0};
    uint8_t hedge{0};  // Hedge; 0 speculates
    uint8_t pad[2]{};
};

struct Pulse {
    uint32_t name{0};
    int64_t stamp_us{0};
    int64_t value{0};
    char topic[24]{};
    uint8_t dense{0};
};

struct TapePrint {
    uint32_t name{0};
    int64_t px{0};
    int64_t qty{0};
    int64_t exch_us{0};
    uint8_t aggressor{0};  // 0 unknown, 1 buy, 2 sell
};

struct QueryNote {
    uint64_t req{0};
    uint32_t acct{0};
    uint32_t engine{0};
    int64_t cursor{0};
    uint8_t kind{0};  // 1 position, 2 asset. last==0 is a request, last==1 is the reply
    uint8_t last{0};
    char locate[24]{};
};

// Full ledger row for a cross-process query reply. The request travels on the
// query lane; each note carries the echoed request, a batch sequence, and a
// last flag. Assets and positions use separate rings so neither frame has to
// hold a union of the two large rows.
struct LedgerAssetNote {
    uint64_t req{0};
    uint32_t acct{0};
    uint32_t seq{0};
    uint8_t last{0};
    uint8_t pad[7]{};
    LedgerAsset row{};
};

struct LedgerPositionNote {
    uint64_t req{0};
    uint32_t acct{0};
    uint32_t seq{0};
    uint8_t last{0};
    uint8_t pad[7]{};
    LedgerPosition row{};
};

// Market-data subscribe lane. code is the instrument text.
enum class SubKind : uint8_t {
    Quote = 1,
    Unquote = 2,
    Signal = 3,
    Rebuild = 4,
    MktOrder = 5,
    MktTrade = 6,
    DropSignals = 7
};

struct SubNote {
    uint32_t name{0};
    uint32_t engine{0};
    uint64_t req{0};
    uint8_t kind{0};
    char code[32]{};
    char topic[24]{};
};

// One signal sample. last==1 closes a batch.
struct SigNote {
    uint32_t name{0};
    int64_t stamp_us{0};
    int64_t value{0};
    int32_t seq{0};
    uint8_t last{0};
    char topic[24]{};
};

// Public order on the tape. act 1 new, 2 cancel.
struct MktNote {
    uint32_t name{0};
    uint64_t seq{0};
    int64_t px{0};
    int64_t qty{0};
    int64_t stamp_us{0};
    uint8_t side{0};
    uint8_t act{0};
    char code[32]{};
};

inline void slot_text(char* dst, int cap, const char* src) noexcept {
    if (!dst || cap <= 0) return;
    int i = 0;
    if (src) {
        for (; src[i] && i + 1 < cap; ++i) dst[i] = src[i];
    }
    dst[i] = 0;
}

// Package lane (local). Body capped; oversized payloads stay on Parcel rings.
struct PackageNote {
    uint64_t id{0};
    uint32_t len{0};
    uint16_t kind{0}; // 1 req, 2 rsp
    uint16_t pad{0};
    uint8_t data[240]{};
};

// Log lane: cold/async drain; hot path only publishes the note.
struct LogNote {
    int32_t level{0};
    int32_t pad{0};
    int64_t stamp_us{0};
    char line[112]{};
};

struct ParcelHead {
    uint64_t id{0};
    uint32_t len{0};
};

template <uint32_t Bytes>
struct Parcel {
    ParcelHead head{};
    uint8_t data[Bytes]{};
};

static_assert(std::is_trivially_copyable<AlgoJob>::value, "AlgoJob");
static_assert(sizeof(AlgoJob) == 256, "AlgoJob stays four cache lines");
static_assert(std::is_trivially_copyable<AlgoRung>::value, "AlgoRung");
static_assert(std::is_trivially_copyable<ExtOrder>::value, "ExtOrder");
static_assert(std::is_trivially_copyable<ControlNote>::value, "ControlNote");
static_assert(std::is_trivially_copyable<ControlAck>::value, "ControlAck");
static_assert(std::is_trivially_copyable<ExecNote>::value, "ExecNote");
static_assert(std::is_trivially_copyable<Pulse>::value, "Pulse");
static_assert(std::is_trivially_copyable<TapePrint>::value, "TapePrint");
static_assert(std::is_trivially_copyable<QueryNote>::value, "QueryNote");
static_assert(std::is_trivially_copyable<LedgerAssetNote>::value, "LedgerAssetNote");
static_assert(std::is_trivially_copyable<LedgerPositionNote>::value, "LedgerPositionNote");
static_assert(std::is_trivially_copyable<SubNote>::value, "SubNote");
static_assert(std::is_trivially_copyable<SigNote>::value, "SigNote");
static_assert(std::is_trivially_copyable<MktNote>::value, "MktNote");
static_assert(std::is_trivially_copyable<PackageNote>::value, "PackageNote");
static_assert(std::is_trivially_copyable<LogNote>::value, "LogNote");
static_assert(std::is_trivially_copyable<Parcel<32>>::value, "Parcel");

template <uint32_t Bytes, uint32_t Cap>
bool publish_parcel(FanRing<Parcel<Bytes>, Cap>& ring, uint64_t id, const void* data, uint32_t len, uint64_t need) noexcept {
    if (len > Bytes) return false;
    Parcel<Bytes> item{};
    item.head.id = id;
    item.head.len = len;
    if (data && len > 0) std::memcpy(item.data, data, len);
    return ring.publish(item, need);
}

}  // namespace pitlane
