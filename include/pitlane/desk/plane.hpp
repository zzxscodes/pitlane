#pragma once

#include <cstdint>

namespace pitlane {

// Four roles on one desk (notes: tape, quote adapter, sim trader, matcher).
// A strategy still sends orders through the desk, the same way it does with a live gateway.

enum class SimRole : uint8_t { Tape = 1, Quote = 2, Trader = 3, Match = 4 };

inline bool role_quotes(SimRole role) noexcept { return role == SimRole::Tape || role == SimRole::Quote; }
inline bool role_match(SimRole role) noexcept { return role == SimRole::Match || role == SimRole::Trader; }

struct DelayPair {
    int64_t submit_ns{0};
    int64_t ack_ns{0};
};

// mode 0: feed delay on both legs. 1: a measured live delay. 2: submit 4× feed, ack 3× feed.
inline DelayPair order_delay(uint8_t mode, int64_t feed_ns, int64_t live_ns) noexcept {
    if (mode == 1) return DelayPair{live_ns, live_ns};
    if (mode == 2) return DelayPair{feed_ns * 4, feed_ns * 3};
    return DelayPair{feed_ns, feed_ns};
}

enum class TapeKind : uint8_t { Spot = 1, Print = 2, BookOrder = 3, Fut = 4, Factor = 5 };

struct BreakSpan {
    int start_sec{0};
    int end_sec{0};
};

struct TapePace {
    double play_x{1};
    double gap_x{40};
    BreakSpan gaps[4]{};
    int ngap{0};
};

inline double tape_speed(const TapePace& pace, int day_sec) noexcept {
    const int n = pace.ngap < 4 ? pace.ngap : 4;
    for (int i = 0; i < n; ++i) {
        if (day_sec >= pace.gaps[i].start_sec && day_sec < pace.gaps[i].end_sec) return pace.gap_x;
    }
    return pace.play_x;
}

inline bool tape_kind_on(TapeKind kind, bool prints, bool factors) noexcept {
    if (kind == TapeKind::Print || kind == TapeKind::BookOrder) return prints;
    if (kind == TapeKind::Factor) return factors;
    return true;
}

struct TapeClock {
    int64_t exch_ns{0};
    int speed{1};
};

inline void follow_clock(TapeClock& local, const TapeClock& master) noexcept {
    local.exch_ns = master.exch_ns;
    local.speed = master.speed;
}

} // namespace pitlane
