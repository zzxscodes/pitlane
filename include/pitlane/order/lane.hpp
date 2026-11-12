#pragma once

#include "pitlane/desk/codec.hpp"

#include <cstdint>

namespace pitlane {

// What one counter channel is willing to accept. The library does not open the channel.
struct LaneRule {
    uint8_t limit{1};
    uint8_t market{0};
    uint8_t open_pos{1};
    uint8_t close_pos{1};
    uint8_t close_today{0};
    uint8_t close_yday{0};
    uint8_t amend{0};
    uint8_t pull{1};
};

inline bool lane_can_amend(const LaneRule& rule) noexcept { return rule.amend != 0; }

inline bool lane_accepts(const LaneRule& rule, PxMode mode, Off off, Act act) noexcept {
    if (act == Act::Pull) return rule.pull != 0;
    if (act != Act::Enter) return false;
    if (mode == PxMode::Limit && rule.limit == 0) return false;
    if (mode == PxMode::Market && rule.market == 0) return false;
    if (mode != PxMode::Limit && mode != PxMode::Market) return false;
    if (off == Off::Open) return rule.open_pos != 0;
    if (off == Off::Close) return rule.close_pos != 0;
    if (off == Off::CloseToday) return rule.close_today != 0;
    if (off == Off::CloseYday) return rule.close_yday != 0;
    return false;
}

// Fixed counter_id → LaneRule. Miss and full table fail-closed.

template <int Cap>
class LaneBook {
public:
    static_assert(Cap >= 1 && Cap <= 256, "cap");

    bool put(uint32_t counter_id, const LaneRule& rule) noexcept {
        if (counter_id == 0) return false;
        const int i = find(counter_id);
        if (i >= 0) {
            row_[i].rule = rule;
            return true;
        }
        for (int j = 0; j < Cap; ++j) {
            if (row_[j].used) continue;
            row_[j].id = counter_id;
            row_[j].rule = rule;
            row_[j].used = 1;
            return true;
        }
        return false;
    }

    const LaneRule* pick(uint32_t counter_id) const noexcept {
        const int i = find(counter_id);
        return i < 0 ? nullptr : &row_[i].rule;
    }

private:
    struct Row {
        uint32_t id{0};
        LaneRule rule{};
        uint8_t used{0};
    };

    int find(uint32_t counter_id) const noexcept {
        for (int i = 0; i < Cap; ++i) {
            if (row_[i].used && row_[i].id == counter_id) return i;
        }
        return -1;
    }

    Row row_[Cap]{};
};

template <int Cap>
inline bool lane_pick(const LaneBook<Cap>& book, uint32_t counter_id, LaneRule& out) noexcept {
    const LaneRule* r = book.pick(counter_id);
    if (!r) return false;
    out = *r;
    return true;
}

// Reject codes share one numbering with a counter. Bands stay stable for monitors.
enum class Cause : uint16_t {
    Ok = 0,
    BadPrice = 10,
    BadQty = 11,
    BadSymbol = 12,
    DupId = 13,
    AlreadyFinal = 14,
    NoCash = 40,
    NoPos = 41,
    NoToday = 42,
    Blocked = 70,
    CashCap = 71,
    OrderCap = 72,
    PullCap = 73,
    PullRatio = 74,
    OrderFlow = 75,
    PullFlow = 76,
    WashRisk = 77,
    Venue = 100,
    Basket = 101
};

enum class CauseBand : uint8_t { Other = 0, Ok = 1, Check = 2, Purse = 3, Risk = 4, External = 5 };

inline CauseBand cause_band(Cause cause) noexcept {
    const int v = static_cast<int>(cause);
    if (v == 0) return CauseBand::Ok;
    if (v >= 100) return CauseBand::External;
    if (v >= 70) return CauseBand::Risk;
    if (v >= 40) return CauseBand::Purse;
    if (v >= 10) return CauseBand::Check;
    return CauseBand::Other;
}

inline const char* cause_text(Cause cause) noexcept {
    switch (cause) {
        case Cause::Ok: return "ok";
        case Cause::BadPrice: return "price";
        case Cause::BadQty: return "qty";
        case Cause::BadSymbol: return "symbol";
        case Cause::DupId: return "dup";
        case Cause::AlreadyFinal: return "final";
        case Cause::NoCash: return "cash";
        case Cause::NoPos: return "pos";
        case Cause::NoToday: return "today";
        case Cause::Blocked: return "blocked";
        case Cause::CashCap: return "cash cap";
        case Cause::OrderCap: return "order cap";
        case Cause::PullCap: return "pull cap";
        case Cause::PullRatio: return "pull ratio";
        case Cause::OrderFlow: return "order flow";
        case Cause::PullFlow: return "pull flow";
        case Cause::WashRisk: return "wash";
        case Cause::Venue: return "venue";
        case Cause::Basket: return "basket";
    }
    return "unknown";
}

}  // namespace pitlane
