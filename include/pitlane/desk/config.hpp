#pragma once

#include "pitlane/desk/broker.hpp"
#include "pitlane/desk/cash.hpp"
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

    // [desk] ring / row caps
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

    // [broker]
    int64_t max_qty{0};
    int64_t max_notional{0};
    int max_live{0};
    int64_t ack_limit_us{0};
    int64_t life_limit_us{0};
    int64_t pull_limit_us{0};
    int broker_watch_cap{BrokerWatch::kCap};

    // [query] compile-time informational
    uint32_t query_engines{QueryFold::kEngines};
    uint32_t query_accounts{QueryFold::kAccounts};
    uint32_t query_queue{QueryFold::kQueue};
    uint32_t query_kinds{QueryFold::kKinds};

    // [host] applied once via conf_apply_host (best-effort; may need privileges)
    int host_cpu{-1};             // pin calling thread; <0 skip
    int host_policy{0};           // 0=skip, 1=FIFO, 2=RR
    int host_priority{0};         // SCHED_* priority when policy set
    int host_nice{0};             // set only when host_set_nice=1
    uint8_t host_set_nice{0};
    int64_t host_hugepages{-1};   // nr_hugepages ensure; <0 skip
    uint8_t host_thp_disable{0};  // 1 → thp_disable()
    uint8_t host_mlockall{0};     // 1 → mem_lock_all()
    int host_numa_node{-1};       // informational / for map default; -1 skip
    char host_irq_list[96]{};     // empty skip; else written to chosen irq via conf (caller supplies irq)

    // [shed] StayShed defaults (caller constructs StayShed with these)
    int64_t shed_target{0};
    int64_t shed_window{0};

    // [pace] TokenBook / Gcra defaults
    int64_t pace_burst{0};
    int64_t pace_rate{0};
    int64_t pace_per{1};
    int64_t pace_idle{0};
    int64_t gcra_interval{0};
    int64_t gcra_tolerance{0};
};

void conf_defaults(PitConf& conf) noexcept;

// err_line receives 1-based line on hard failure when non-null.
bool conf_load(const char* path, PitConf& conf) noexcept;
bool conf_load_ex(const char* path, PitConf& conf, int* err_line) noexcept;

int conf_normalize(PitConf& conf, char* hints, int hints_cap) noexcept;

MapOpt conf_map_opt(const PitConf& conf) noexcept;
ArenaOpt conf_arena_opt(const PitConf& conf) noexcept;
RiskCaps conf_risk_caps(const PitConf& conf) noexcept;

bool desk_apply_conf(Desk& desk, const PitConf& conf) noexcept;
bool cash_apply_conf(CashBoard& board, const PitConf& conf) noexcept;
bool hold_apply_conf(HoldBoard& board, const PitConf& conf) noexcept;

// Best-effort host apply. Returns number of successful actions; writes hints for skips/fails.
int conf_apply_host(const PitConf& conf, char* hints, int hints_cap) noexcept;

}  // namespace pitlane
