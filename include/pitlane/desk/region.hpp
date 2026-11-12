#pragma once

#include "pitlane/desk/cash.hpp"
#include "pitlane/desk/ring.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/desk/wire.hpp"
#include "pitlane/util/hint.hpp"

#include <cstddef>
#include <cstdint>

namespace pitlane {

// One shared region for several processes.
// Each ring has a single writer. Readers keep their own cursors.
// Cash and inventory rows stay in place and are updated under RowGate.
//
// Explicit huge pages only: when MapOpt::hugepage is set, map_create / map_attach
// place the file on hugetlbfs (default /dev/hugepages) or use a path already on
// that mount. Ordinary mounts never get MADV_HUGEPAGE / THP. If explicit pages
// are unavailable the mapping falls back to the caller path and sets fallback.
// Optional mlock and NUMA bind apply after a successful map. Desk layout is
// FanRing claim state bumps the on-disk layout; kDeskVersion is 3.

inline constexpr uint32_t kIntentCap = 1024;
inline constexpr uint32_t kReportCap = 1024;
inline constexpr uint32_t kQuoteCap = 2048;
inline constexpr uint32_t kCashCap = 256;
inline constexpr uint32_t kHoldCap = 256;
inline constexpr uint32_t kAlgoCap = 256;
inline constexpr uint32_t kControlCap = 256;
inline constexpr uint32_t kPulseCap = 512;
inline constexpr uint32_t kPrintCap = 512;
inline constexpr uint32_t kQueryCap = 256;
inline constexpr uint32_t kDeskVersion = 3;

struct alignas(64) Desk {
    FanRing<Intent, kIntentCap> intents;
    FanRing<Report, kReportCap> reports;
    FanRing<Quote, kQuoteCap> quotes;
    FanRing<AlgoJob, kAlgoCap> algos;
    FanRing<ControlNote, kControlCap> controls;
    FanRing<Pulse, kPulseCap> pulses;
    FanRing<TapePrint, kPrintCap> prints;
    FanRing<QueryNote, kQueryCap> queries;
    CashLine cash[kCashCap];
    HoldLine hold[kHoldCap];
    alignas(64) std::atomic<int64_t> clock_us{0};
    char magic[8]{};
    uint32_t version{0};
};

struct MapOpt {
    uint8_t hugepage{1};                 // explicit hugetlbfs / no THP
    uint8_t lock{1};                     // try mlock after map
    uint8_t prefault{1};                 // touch every page after map
    int8_t numa_node{-1};                // >=0: MPOL_BIND after map
    const char* hugepage_dir{"/dev/hugepages"};
};

struct MapInfo {
    void* addr{nullptr};
    std::size_t bytes{0};
    int fd{-1};
    uint8_t hugepage{0};   // 1 when the mapping is explicit huge pages
    uint8_t locked{0};
    uint8_t fallback{0};   // 1 when hugepage was requested but ordinary pages used
    int8_t numa_node{-1};  // node after successful bind, else -1
};

std::size_t desk_bytes() noexcept;

// Creates or extends a file-backed mapping. Does not wipe an existing region.
int map_create(const char* path, std::size_t bytes, MapInfo* out, MapOpt opt = {}) noexcept;

// Attaches an existing file. Fails if the file is shorter than bytes.
int map_attach(const char* path, std::size_t bytes, MapInfo* out, MapOpt opt = {}) noexcept;

void map_release(MapInfo* io) noexcept;

// Placement-constructs a fresh desk. Call this only on a zeroed or abandoned mapping.
PITLANE_COLD void desk_format(Desk* desk) noexcept;

// Returns the desk inside the mapping.
// create_if_empty formats the region when the magic is still zero.
// A mapping with an unexpected magic is left untouched and yields nullptr.
Desk* desk_view(MapInfo* io, int create_if_empty) noexcept;

}  // namespace pitlane
