#pragma once

#include "pitlane/desk/broker.hpp"
#include "pitlane/desk/region.hpp"
#include "pitlane/util/arena.hpp"
#include "pitlane/util/hint.hpp"

#include <cstddef>
#include <cstdint>

namespace pitlane {

// Startup configuration. Defaults match the compile-time constants in headers.
// Load once before map/desk open. Hot paths never read the file again.
// Omitted / empty keys keep defaults. normalize() clamps and may rewrite values,
// writing human-readable hints into the caller buffer (no heap).

struct PitConf {
    // [map]
    uint8_t map_hugepage{1};
    uint8_t map_lock{1};
    uint8_t map_prefault{1};
    int8_t map_numa_node{-1};
    char hugepage_dir[160]{"/dev/hugepages"};

    // [arena]
    uint8_t arena_hugepage{1};
    uint8_t arena_lock{1};
    uint8_t arena_prefault{1};
    int8_t arena_numa_node{-1};

    // [desk] ring / row caps — must be powers of two where rings require it;
    // values above compile-time maxima are clamped down.
    uint32_t intent_cap{kIntentCap};
    uint32_t report_cap{kReportCap};
    uint32_t quote_cap{kQuoteCap};
    uint32_t algo_cap{kAlgoCap};
    uint32_t control_cap{kControlCap};
    uint32_t pulse_cap{kPulseCap};
    uint32_t print_cap{kPrintCap};
    uint32_t query_cap{kQueryCap};
    uint32_t cash_cap{kCashCap};
    uint32_t hold_cap{kHoldCap};

    // [broker] soft limits (0 = axis off), used as RiskCaps / expire defaults
    int64_t max_qty{0};
    int64_t max_notional{0};
    int max_live{0};
    int64_t ack_limit_us{0};
    int64_t life_limit_us{0};
    int64_t pull_limit_us{0};

    // [query] informational — compile-time QueryFold sizes; mismatch only warns
    uint32_t query_engines{QueryFold::kEngines};
    uint32_t query_accounts{QueryFold::kAccounts};
    uint32_t query_queue{QueryFold::kQueue};
    uint32_t query_kinds{QueryFold::kKinds};

    // [broker_watch]
    int broker_watch_cap{BrokerWatch::kCap};
};

// Fills conf with built-in defaults (same as struct member initializers).
void conf_defaults(PitConf& conf) noexcept;

// Load key=value / [section] file. Missing file or empty path leaves defaults.
// Returns false only on hard parse failure (truncated buffer, forbidden control chars).
bool conf_load(const char* path, PitConf& conf) noexcept;

// Clamp / round to safe values. Appends hint lines into hints (cap bytes). Returns hint count.
int conf_normalize(PitConf& conf, char* hints, int hints_cap) noexcept;

MapOpt conf_map_opt(const PitConf& conf) noexcept;
ArenaOpt conf_arena_opt(const PitConf& conf) noexcept;
RiskCaps conf_risk_caps(const PitConf& conf) noexcept;

// Apply desk ring soft capacities (startup only, empty desk). Returns false if tune fails.
bool desk_apply_conf(Desk& desk, const PitConf& conf) noexcept;

}  // namespace pitlane
