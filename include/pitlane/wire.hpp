#pragma once

#include "pitlane/ring.hpp"

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace pitlane {

// Bounded records carried on the extra desk lanes.
// Prices and quantities are integers. Times are microseconds.

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
};

struct ControlNote {
    uint64_t tag{0};
    uint32_t name{0};
    int64_t stamp_us{0};
    uint8_t kind{0};  // 1 stop, 2 reload, 3 pause, 4 resume, 5 new date, 6 new parameter
    char text[48]{};
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
    uint8_t kind{0};  // 1 inventory, 2 cash
    uint8_t last{0};
    char locate[24]{};
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
static_assert(std::is_trivially_copyable<ControlNote>::value, "ControlNote");
static_assert(std::is_trivially_copyable<Pulse>::value, "Pulse");
static_assert(std::is_trivially_copyable<TapePrint>::value, "TapePrint");
static_assert(std::is_trivially_copyable<QueryNote>::value, "QueryNote");
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
