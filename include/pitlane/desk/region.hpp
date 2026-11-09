#pragma once

#include "pitlane/desk/cash.hpp"
#include "pitlane/desk/ring.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/desk/wire.hpp"

#include <cstddef>
#include <cstdint>

namespace pitlane {

// One shared region for several processes.
// Each ring has a single writer. Readers keep their own cursors.
// Cash and inventory rows stay in place and are updated under RowGate.

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
inline constexpr uint32_t kDeskVersion = 2;

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

struct MapInfo {
    void* addr{nullptr};
    std::size_t bytes{0};
    int fd{-1};
};

std::size_t desk_bytes() noexcept;

// Creates or extends a file-backed mapping. Does not wipe an existing region.
int map_create(const char* path, std::size_t bytes, MapInfo* out) noexcept;

// Attaches an existing file. Fails if the file is shorter than bytes.
int map_attach(const char* path, std::size_t bytes, MapInfo* out) noexcept;

void map_release(MapInfo* io) noexcept;

// Placement-constructs a fresh desk. Call this only on a zeroed or abandoned mapping.
void desk_format(Desk* desk) noexcept;

// Returns the desk inside the mapping.
// create_if_empty formats the region when the magic is still zero.
// A mapping with an unexpected magic is left untouched and yields nullptr.
Desk* desk_view(MapInfo* io, int create_if_empty) noexcept;

}  // namespace pitlane
