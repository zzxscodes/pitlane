#pragma once

#include "pitlane/time/mag.hpp"
#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstdint>

namespace pitlane {

// Per-lane tail telemetry. Each lane is single-writer; readers can call
// tail_report or read max_cycles at any time. The histogram is TSC cycles, not
// wall time, so it stays meaningful when a clock source drifts or is replaced.
//
// The trigger is the in-process half of a magic-trace style capture: when a
// sample crosses limit_cycles, the board freezes the tape so a sidecar can dump
// the last N events without stopping the hot path.

enum class TailLane : uint8_t {
    Intent = 0,
    Report = 1,
    Quote = 2,
    Signal = 3,
    Algo = 4,
    Print = 5,
    Control = 6,
    Recover = 7,
    kCount = 8
};

inline constexpr int kTailLanes = static_cast<int>(TailLane::kCount);

struct TailLaneStat {
    MagHist wait{};
    MagHist gap{};
    uint64_t notes{0};
    uint64_t stalls{0};
    uint64_t overruns{0};
    uint64_t max_cycles{0};
};

struct TailTrigger {
    uint64_t limit_cycles{0};
    uint32_t lane_mask{0};
    uint32_t pad0{0};
    std::atomic<uint8_t> fired{0};
    std::atomic<uint64_t> at_cycles{0};
};

struct TailBoard {
    TailLaneStat lane[kTailLanes]{};
    TailTrigger trigger{};
    uint64_t budget_cycles{0};
};

inline bool tail_armed(const TailBoard& board) noexcept {
    return board.trigger.limit_cycles != 0 && board.trigger.lane_mask != 0;
}

inline bool tail_fired(const TailBoard& board) noexcept {
    return board.trigger.fired.load(std::memory_order_acquire) != 0;
}

// Records one sample. Returns true only on the sample that fires the trigger.
PITLANE_HOT inline bool tail_note(TailBoard& board, TailLane lane, uint64_t cycles) noexcept {
    const int i = static_cast<int>(lane);
    if (unlikely(i < 0 || i >= kTailLanes)) return false;
    TailLaneStat& stat = board.lane[i];
    if (unlikely(stat.notes == UINT64_MAX)) return false;
    ++stat.notes;
    if (cycles > stat.max_cycles) stat.max_cycles = cycles;
    if (!stat.wait.add(cycles)) ++stat.overruns;
    if (board.budget_cycles != 0 && cycles > board.budget_cycles) ++stat.stalls;

    TailTrigger& t = board.trigger;
    if (unlikely(t.limit_cycles == 0 || (t.lane_mask & (1u << i)) == 0u)) return false;
    if (cycles < t.limit_cycles) return false;
    uint8_t expected = 0;
    if (t.fired.compare_exchange_strong(expected, 1, std::memory_order_release,
                                        std::memory_order_relaxed)) {
        t.at_cycles.store(cycles, std::memory_order_release);
        return true;
    }
    return false;
}

PITLANE_HOT inline bool tail_gap(TailBoard& board, TailLane lane, uint64_t cycles) noexcept {
    const int i = static_cast<int>(lane);
    if (unlikely(i < 0 || i >= kTailLanes)) return false;
    return board.lane[i].gap.add(cycles);
}

inline bool tail_report(const TailBoard& board, TailLane lane, LatReport& out) noexcept {
    const int i = static_cast<int>(lane);
    if (i < 0 || i >= kTailLanes) return false;
    return mag_report(board.lane[i].wait, out);
}

inline bool tail_gap_report(const TailBoard& board, TailLane lane, LatReport& out) noexcept {
    const int i = static_cast<int>(lane);
    if (i < 0 || i >= kTailLanes) return false;
    return mag_report(board.lane[i].gap, out);
}

inline void tail_arm(TailBoard& board, uint64_t limit_cycles, uint32_t lane_mask) noexcept {
    board.trigger.limit_cycles = limit_cycles;
    board.trigger.lane_mask = lane_mask;
    board.trigger.fired.store(0, std::memory_order_release);
    board.trigger.at_cycles.store(0, std::memory_order_release);
}

inline void tail_reset(TailBoard& board) noexcept {
    for (int i = 0; i < kTailLanes; ++i) board.lane[i] = TailLaneStat{};
    board.trigger.fired.store(0, std::memory_order_release);
    board.trigger.at_cycles.store(0, std::memory_order_release);
}

// A small frozen-on-trigger event tape. It is independent of the desk rings so
// a sidecar can read it after the trigger without holding up a producer.
template <uint32_t Cap>
class TailTape {
public:
    static_assert((Cap & (Cap - 1u)) == 0u && Cap >= 2u, "tail tape cap");

    struct Rec {
        uint64_t tsc{0};
        uint64_t value{0};
        uint16_t lane{0};
        uint16_t pad{0};
        uint32_t seq{0};
    };

    PITLANE_HOT void note(uint16_t lane, uint64_t tsc, uint64_t value) noexcept {
        if (frozen_.load(std::memory_order_relaxed) != 0) return;
        const uint64_t head = head_.load(std::memory_order_relaxed);
        Rec& rec = rec_[head & (Cap - 1u)];
        rec.tsc = tsc;
        rec.value = value;
        rec.lane = lane;
        rec.pad = 0;
        rec.seq = static_cast<uint32_t>(head + 1u);
        head_.store(head + 1u, std::memory_order_release);
    }

    bool freeze() noexcept {
        uint8_t expected = 0;
        return frozen_.compare_exchange_strong(expected, 1, std::memory_order_release,
                                               std::memory_order_relaxed);
    }

    bool frozen() const noexcept { return frozen_.load(std::memory_order_acquire) != 0; }
    uint64_t written() const noexcept { return head_.load(std::memory_order_acquire); }

    uint32_t count() const noexcept {
        const uint64_t head = written();
        return static_cast<uint32_t>(head < Cap ? head : Cap);
    }

    bool at(uint32_t i, Rec& out) const noexcept {
        if (i >= count()) return false;
        const uint64_t head = written();
        const uint64_t pos = head - 1u - i; // newest first
        out = rec_[pos & (Cap - 1u)];
        return true;
    }

private:
    Rec rec_[Cap]{};
    std::atomic<uint64_t> head_{0};
    std::atomic<uint8_t> frozen_{0};
};

} // namespace pitlane
