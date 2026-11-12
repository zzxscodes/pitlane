#pragma once

#include "pitlane/desk/region.hpp"
#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pitlane {

// Symbol / quote yard: startup-sized shared layout role-aligned with the reference design
// startup pool pools. Pitlane-owned names and integer hot path. All slots and
// tape cells are reserved in yard_open. Hot path never allocates. Fail-closed.

inline constexpr uint32_t kYardVersion = 2;
inline constexpr uint32_t kYardSymSlotsDefault = 150000;
inline constexpr uint32_t kYardQuoteLanesDefault = 150000;
inline constexpr uint32_t kYardQuoteDepthDefault = 256;
inline constexpr uint32_t kYardOrderLanesDefault = 10;
inline constexpr uint32_t kYardTradeLanesDefault = 10;
inline constexpr uint32_t kYardRebuildLanesDefault = 10;
inline constexpr uint32_t kYardMixLanesDefault = 10;
inline constexpr uint32_t kYardOrderDepthDefault = 32768;
inline constexpr uint32_t kYardTradeDepthDefault = 32768;
inline constexpr uint32_t kYardRebuildDepthDefault = 128;
inline constexpr uint32_t kYardMixDepthDefault = 32768;
inline constexpr uint32_t kYardBaseInfoDefault = 35000;
inline constexpr uint32_t kYardTradeTimeDefault = 500;

struct YardPlan {
    uint32_t sym_slots{kYardSymSlotsDefault};
    uint32_t quote_lanes{kYardQuoteLanesDefault};
    uint32_t quote_depth{kYardQuoteDepthDefault}; // power of two
    uint32_t order_lanes{kYardOrderLanesDefault};
    uint32_t trade_lanes{kYardTradeLanesDefault};
    uint32_t rebuild_lanes{kYardRebuildLanesDefault};
    uint32_t mix_lanes{kYardMixLanesDefault};
    uint32_t order_depth{kYardOrderDepthDefault}; // power of two
    uint32_t trade_depth{kYardTradeDepthDefault}; // power of two
    uint32_t rebuild_depth{kYardRebuildDepthDefault}; // power of two
    uint32_t mix_depth{kYardMixDepthDefault}; // power of two
    uint32_t base_info_cap{kYardBaseInfoDefault};
    uint32_t trade_time_cap{kYardTradeTimeDefault};
};

struct YardMark {
    int64_t bid_px{0};
    int64_t ask_px{0};
    int64_t bid_qty{0};
    int64_t ask_qty{0};
    int64_t ts_us{0};
    uint64_t seq{0};
};

// Book-order tape cell (MemLay ExtOrder role).
struct YardTick {
    int64_t px{0};
    int64_t qty{0};
    int64_t ts_us{0};
    uint8_t side{0}; // 1 buy, 2 sell
    uint8_t kind{0};
    uint64_t seq{0};
};

// Trade tape cell (MemLay ExtTrade role).
struct YardPrint {
    int64_t px{0};
    int64_t qty{0};
    int64_t ts_us{0};
    uint8_t side{0};
    uint64_t seq{0};
};

// Static contract facts (MemLay InstrumentBaseInfo role).
struct YardFacts {
    char code[32]{};
    int64_t tick_sz{0};
    int64_t lot_sz{0};
    int64_t mult{0};
    int32_t hours_id{0}; // 1-based YardHours id; 0 none
    uint8_t live{0};
};

// Session hours row (MemLay TradeTimeInfo role). Times are seconds from midnight.
struct YardHours {
    char key[32]{};
    int32_t open_sec{0};
    int32_t close_sec{0};
    int32_t open2_sec{0};
    int32_t close2_sec{0};
    uint8_t live{0};
};

struct YardSym {
    char code[32]{};
    uint32_t id{0}; // 1-based dense id; 0 empty
    int32_t quote_lane{-1};
    int32_t order_lane{-1};
    int32_t trade_lane{-1};
    int32_t rebuild_lane{-1};
    int32_t mix_lane{-1};
    int32_t facts_id{0}; // 1-based YardFacts; 0 none
    int32_t hours_id{0}; // 1-based YardHours; 0 none
    uint8_t live{0};
};

struct YardHead {
    char magic[8]{};
    uint32_t version{0};
    YardPlan plan{};
    alignas(64) std::atomic<uint32_t> sym_used{0};
    alignas(64) std::atomic<uint32_t> quote_free_top{0};
    alignas(64) std::atomic<uint32_t> order_free_top{0};
    alignas(64) std::atomic<uint32_t> trade_free_top{0};
    alignas(64) std::atomic<uint32_t> rebuild_free_top{0};
    alignas(64) std::atomic<uint32_t> mix_free_top{0};
    alignas(64) std::atomic<uint32_t> facts_used{0};
    alignas(64) std::atomic<uint32_t> hours_used{0};
};

struct YardView {
    MapInfo map{};
    YardHead* head{nullptr};
    YardSym* syms{nullptr};
    std::atomic<uint64_t>* quote_pub{nullptr};
    std::atomic<uint64_t>* quote_ack{nullptr};
    YardMark* quote_cells{nullptr};
    int32_t* quote_free{nullptr};
    std::atomic<uint64_t>* order_pub{nullptr};
    YardTick* order_cells{nullptr};
    int32_t* order_free{nullptr};
    std::atomic<uint64_t>* trade_pub{nullptr};
    YardPrint* trade_cells{nullptr};
    int32_t* trade_free{nullptr};
    std::atomic<uint64_t>* rebuild_pub{nullptr};
    YardMark* rebuild_cells{nullptr};
    int32_t* rebuild_free{nullptr};
    std::atomic<uint64_t>* mix_pub{nullptr};
    YardMark* mix_cells{nullptr};
    int32_t* mix_free{nullptr};
    YardFacts* facts{nullptr};
    YardHours* hours{nullptr};
};

std::size_t yard_bytes(const YardPlan& plan) noexcept;
bool yard_plan_ok(const YardPlan& plan) noexcept;

int yard_open(const char* path, const YardPlan& plan, YardView* out, MapOpt opt = {}, int create = 1) noexcept;
void yard_close(YardView* io) noexcept;

uint32_t yard_claim_sym(YardView& y, const char* code) noexcept;
YardSym* yard_sym(YardView& y, uint32_t id) noexcept;
const YardSym* yard_sym(const YardView& y, uint32_t id) noexcept;

bool yard_bind_quote(YardView& y, uint32_t id) noexcept;
bool yard_bind_order(YardView& y, uint32_t id) noexcept;
bool yard_bind_trade(YardView& y, uint32_t id) noexcept;
bool yard_bind_rebuild(YardView& y, uint32_t id) noexcept;
bool yard_bind_mix(YardView& y, uint32_t id) noexcept;

PITLANE_HOT bool yard_publish(YardView& y, uint32_t id, const YardMark& mark) noexcept;
PITLANE_HOT bool yard_latest(const YardView& y, uint32_t id, YardMark& out) noexcept;

PITLANE_HOT bool yard_publish_tick(YardView& y, uint32_t id, const YardTick& tick) noexcept;
PITLANE_HOT bool yard_latest_tick(const YardView& y, uint32_t id, YardTick& out) noexcept;

PITLANE_HOT bool yard_publish_print(YardView& y, uint32_t id, const YardPrint& print) noexcept;
PITLANE_HOT bool yard_latest_print(const YardView& y, uint32_t id, YardPrint& out) noexcept;

PITLANE_HOT bool yard_publish_rebuild(YardView& y, uint32_t id, const YardMark& mark) noexcept;
PITLANE_HOT bool yard_latest_rebuild(const YardView& y, uint32_t id, YardMark& out) noexcept;

PITLANE_HOT bool yard_publish_mix(YardView& y, uint32_t id, const YardMark& mark) noexcept;
PITLANE_HOT bool yard_latest_mix(const YardView& y, uint32_t id, YardMark& out) noexcept;

// Cold path: claim / link static tables.
uint32_t yard_claim_facts(YardView& y, const char* code) noexcept;
YardFacts* yard_facts(YardView& y, uint32_t facts_id) noexcept;
const YardFacts* yard_facts(const YardView& y, uint32_t facts_id) noexcept;
bool yard_link_facts(YardView& y, uint32_t sym_id, uint32_t facts_id) noexcept;

uint32_t yard_claim_hours(YardView& y, const char* key) noexcept;
YardHours* yard_hours(YardView& y, uint32_t hours_id) noexcept;
const YardHours* yard_hours(const YardView& y, uint32_t hours_id) noexcept;
bool yard_link_hours(YardView& y, uint32_t sym_id, uint32_t hours_id) noexcept;

} // namespace pitlane
