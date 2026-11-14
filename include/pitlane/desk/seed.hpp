#pragma once

#include "pitlane/desk/cash.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <cstring>

namespace pitlane {

// Cold seed rows (accounts / position.json style). Caller fills
// arrays at startup; apply once before hot path. No heap.

struct CashSeed {
    uint8_t tier{0};
    char prod[16]{};
    char acct[24]{};
    char desk[16]{};
    char book[16]{};
    int64_t free_amt{0};
};

struct HoldSeed {
    uint8_t tier{0};
    char prod[16]{};
    char acct[24]{};
    char name[16]{};
    int64_t long_today{0};
    int64_t long_yday{0};
    int64_t short_today{0};
    int64_t short_yday{0};
    int64_t sellable{0};
    int64_t frozen{0};
    int64_t frozen_yday{0};
    uint8_t split_yday{0};
};

inline bool seed_copy16(char* dst, const char* src) noexcept {
    if (!dst) return false;
    if (!src) {
        dst[0] = 0;
        return true;
    }
    int n = 0;
    for (; src[n] && n < 15; ++n) dst[n] = src[n];
    if (src[n]) return false;
    dst[n] = 0;
    return true;
}

inline bool seed_copy24(char* dst, const char* src) noexcept {
    if (!dst) return false;
    if (!src) {
        dst[0] = 0;
        return true;
    }
    int n = 0;
    for (; src[n] && n < 23; ++n) dst[n] = src[n];
    if (src[n]) return false;
    dst[n] = 0;
    return true;
}

// Returns number applied, or -1 if any upsert fails (fail-closed; prior rows kept).
inline int seed_cash(CashBoard& board, const CashSeed* rows, int n) noexcept {
    if (!rows || n < 0) return -1;
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        CashLine* line = board.upsert(rows[i].tier, rows[i].prod, rows[i].acct, rows[i].desk, rows[i].book);
        if (!line) return -1;
        line->gate.enter();
        line->free_amt = rows[i].free_amt;
        line->held_amt = 0;
        line->live.store(1, std::memory_order_release);
        line->gate.leave();
        ++ok;
    }
    return ok;
}

inline int seed_hold(HoldBoard& board, const HoldSeed* rows, int n) noexcept {
    if (!rows || n < 0) return -1;
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        HoldLine* line =
            board.upsert(rows[i].tier, rows[i].prod, rows[i].acct, rows[i].name, rows[i].split_yday != 0);
        if (!line) return -1;
        line->gate.enter();
        line->long_today = rows[i].long_today;
        line->long_yday = rows[i].long_yday;
        line->short_today = rows[i].short_today;
        line->short_yday = rows[i].short_yday;
        line->sellable = rows[i].sellable;
        line->frozen = rows[i].frozen;
        line->frozen_yday = rows[i].frozen_yday;
        line->split_yday = rows[i].split_yday;
        line->live.store(1, std::memory_order_release);
        line->gate.leave();
        ++ok;
    }
    return ok;
}

} // namespace pitlane
