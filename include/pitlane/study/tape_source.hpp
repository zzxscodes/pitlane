#pragma once

// Tape source adapters. A source is a plain function pointer so callers can
// switch between a block tape, a CSV dump, or a synthetic generator without
// virtual dispatch or allocation. Cold path only.

#include "pitlane/study/block_tape.hpp"

#include <cstdint>

namespace pitlane {

// Loads a whole tape version into `out`. `version` receives the version that
// was read (0 means the latest). Returns false when the file is missing, is not
// this source's format, or does not fit in `cap`.
using TapeLoadFn = bool (*)(const char* path, Quote* out, uint32_t cap, uint32_t& n, uint32_t& version) noexcept;

struct TapeSourceApi {
    const char* name;
    TapeLoadFn load;
};

inline bool tape_load_block(const char* path, Quote* out, uint32_t cap, uint32_t& n, uint32_t& version) noexcept {
    n = 0;
    version = 0;
    if (!path || !path[0] || !out || cap == 0) return false;
    BlockTapeReader r;
    if (!r.open(path)) return false;
    if (!r.load_quotes(0, out, cap, n)) return false;
    TapeMeta m{};
    if (r.meta(m)) version = m.versions;
    return n > 0;
}

// Reads only the latest `cap` ticks, which is enough for live-style replay.
inline bool tape_tail_block(const char* path, Quote* out, uint32_t cap, uint32_t& n, uint32_t& version) noexcept {
    n = 0;
    version = 0;
    if (!path || !path[0] || !out || cap == 0) return false;
    BlockTapeReader r;
    if (!r.open(path)) return false;
    TapeMeta m{};
    if (!r.meta(m)) return false;
    const uint64_t rows = m.rows;
    const uint32_t offset = rows > cap ? static_cast<uint32_t>(rows - cap) : 0;
    TapeTick tmp[512];
    uint32_t got = 0;
    uint32_t written = 0;
    while (written < cap) {
        const uint32_t want = (cap - written) < 512 ? (cap - written) : 512;
        if (!r.rows(0, INT64_MIN, INT64_MAX, offset + written, want, kTapeColAll, tmp, want, got) || got == 0) break;
        for (uint32_t i = 0; i < got; ++i) {
            Quote q{};
            q.name = m.symbol;
            q.exch_us = tmp[i].exch_us;
            q.local_us = tmp[i].local_us;
            q.bid_px = tmp[i].bid_px;
            q.ask_px = tmp[i].ask_px;
            q.last_px = tmp[i].last_px;
            q.bid_qty = tmp[i].bid_qty;
            q.ask_qty = tmp[i].ask_qty;
            q.last_qty = tmp[i].last_qty;
            out[written + i] = q;
        }
        written += got;
        if (got < want) break;
    }
    n = written;
    version = m.versions;
    return n > 0;
}

} // namespace pitlane
