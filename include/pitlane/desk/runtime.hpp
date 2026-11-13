#pragma once

#include "pitlane/desk/region.hpp"

#include <cstdint>

namespace pitlane {

inline constexpr unsigned kLoopReader = 7;
inline constexpr unsigned kStratReader = kLoopReader;
inline constexpr unsigned kGateReader = 6;  // trader gateway
inline constexpr unsigned kMdReader = 5;    // market-data gateway

struct LoopHooks {
    void (*on_quote)(void* user, const Quote& quote){nullptr};
    void (*on_report)(void* user, const Report& report){nullptr};
    void (*on_pulse)(void* user, const Pulse& pulse){nullptr};
    void (*on_print)(void* user, const TapePrint& print){nullptr};
    void (*on_algo)(void* user, const AlgoJob& job){nullptr};
    void (*on_query)(void* user, const QueryNote& note){nullptr};
    void (*on_pack_req)(void* user, const PackageNote& note){nullptr};
    void (*on_pack_rsp)(void* user, const PackageNote& note){nullptr};
    void (*on_log)(void* user, const LogNote& note){nullptr};
    void (*on_control)(void* user, const ControlNote& note){nullptr};
    void (*on_timer)(void* user, uint64_t timer_id, int64_t now_us){nullptr};
    void* user{nullptr};
};

class DeskLoop {
public:
    static constexpr int kTimers = 32;

    bool add_timer(uint64_t id, int64_t when_us, int64_t every_us) noexcept;
    void request_stop() noexcept { stop_ = 1; }
    bool stopped() const noexcept { return stop_ != 0; }
    int beat(Desk& desk, int64_t now_us, const LoopHooks& hooks) noexcept;

private:
    struct Timer {
        uint64_t id{0};
        int64_t when_us{0};
        int64_t every_us{0};
        uint8_t live{0};
    };
    Timer timer_[kTimers]{};
    uint64_t quote_cur_{0};
    uint64_t report_cur_{0};
    uint64_t pulse_cur_{0};
    uint64_t print_cur_{0};
    uint64_t algo_cur_{0};
    uint64_t query_cur_{0};
    uint64_t pack_req_cur_{0};
    uint64_t pack_rsp_cur_{0};
    uint64_t log_cur_{0};
    uint64_t control_cur_{0};
    uint8_t stop_{0};

    template <typename T, uint32_t Cap>
    int drain(FanRing<T, Cap>& ring, uint64_t& cursor, void (*fn)(void*, const T&), void* user) noexcept;
};

}  // namespace pitlane
