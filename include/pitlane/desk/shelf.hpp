#pragma once

#include "pitlane/book/calendar.hpp"
#include "pitlane/desk/yard.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace pitlane {

// Local stand-in for Mongo basic_data tables (security_info / trade_time_info /
// holiday_info). Caller fills rows at startup; no network, no heap on apply.

struct FactSeed {
    char code[32]{};
    int64_t tick_sz{0};
    int64_t lot_sz{0};
    int64_t mult{0};
    char hours_key[32]{}; // empty → skip hours link
};

struct HoursSeed {
    char key[32]{};
    int32_t open_sec{0};
    int32_t close_sec{0};
    int32_t open2_sec{0};
    int32_t close2_sec{0};
};

inline bool shelf_copy32(char* dst, const char* src) noexcept {
    if (!dst) return false;
    if (!src) {
        dst[0] = 0;
        return true;
    }
    int n = 0;
    for (; src[n] && n < 31; ++n) dst[n] = src[n];
    if (src[n]) return false;
    dst[n] = 0;
    return true;
}

// Claim / fill / link YardFacts (+ optional hours). Returns applied count, or -1.
inline int seed_facts(YardView& y, const FactSeed* rows, int n) noexcept {
    if (!rows || n < 0) return -1;
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        if (!rows[i].code[0]) return -1;
        const uint32_t fid = yard_claim_facts(y, rows[i].code);
        if (fid == 0) return -1;
        YardFacts* f = yard_facts(y, fid);
        if (!f) return -1;
        f->tick_sz = rows[i].tick_sz;
        f->lot_sz = rows[i].lot_sz;
        f->mult = rows[i].mult;
        const uint32_t sid = yard_claim_sym(y, rows[i].code);
        if (sid == 0) return -1;
        if (!yard_link_facts(y, sid, fid)) return -1;
        if (rows[i].hours_key[0]) {
            const uint32_t hid = yard_claim_hours(y, rows[i].hours_key);
            if (hid == 0) return -1;
            f->hours_id = static_cast<int32_t>(hid);
            if (!yard_link_hours(y, sid, hid)) return -1;
        }
        ++ok;
    }
    return ok;
}

inline int seed_hours(YardView& y, const HoursSeed* rows, int n) noexcept {
    if (!rows || n < 0) return -1;
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        if (!rows[i].key[0]) return -1;
        const uint32_t hid = yard_claim_hours(y, rows[i].key);
        if (hid == 0) return -1;
        YardHours* h = yard_hours(y, hid);
        if (!h) return -1;
        h->open_sec = rows[i].open_sec;
        h->close_sec = rows[i].close_sec;
        h->open2_sec = rows[i].open2_sec;
        h->close2_sec = rows[i].close2_sec;
        ++ok;
    }
    return ok;
}

inline int seed_holidays(ClosedDays& days, const int* ymds, int n) noexcept {
    if (!ymds || n < 0) return -1;
    int ok = 0;
    for (int i = 0; i < n; ++i) {
        if (!ymd_valid(ymds[i])) return -1;
        if (!days.add(ymds[i])) return -1;
        ++ok;
    }
    return ok;
}

// Cold file loader: one YYYYMMDD per line (# comments / blanks skipped). Startup only.
inline int load_holidays(ClosedDays& days, const char* path) noexcept {
    if (!path || !path[0]) return -1;
    FILE* f = std::fopen(path, "r");
    if (!f) return -1;
    char line[64];
    int ok = 0;
    while (std::fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#' || *p == ';' || *p == '\n' || *p == '\r') continue;
        int ymd = 0;
        for (; *p >= '0' && *p <= '9'; ++p) ymd = ymd * 10 + (*p - '0');
        if (!ymd_valid(ymd) || !days.add(ymd)) {
            std::fclose(f);
            return -1;
        }
        ++ok;
    }
    std::fclose(f);
    return ok;
}

} // namespace pitlane
