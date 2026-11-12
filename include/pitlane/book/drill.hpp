#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Deterministic paper drills (paper matcher forced fill / session state role).
// Pitlane-owned names. Fixed table, fail-closed when full. No heap.

enum class DrillCue : uint8_t {
    None = 0,
    FillAll = 1,     // force full fill
    FillTwice = 2,   // two fills covering qty
    Kill = 3,        // force cancel
    KillLeft = 4,    // fill part then cancel rest
    Deny = 5,        // emit Denied
    HangEnter = 6,   // stay staged/working; on_clock skips
    HangPull = 7,    // stay in PullWait; on_clock skips
    DropFill = 8,    // apply fill qty without Report
    Closed = 10      // NotTrading / session closed (session state); sets trade_open=0
};

struct DrillRow {
    uint64_t tag{0};
    DrillCue cue{DrillCue::None};
    int64_t px{0};   // fill px when needed; 0 → ticket px
    int64_t qty{0};  // for KillLeft / DropFill partial; 0 → all leaves
};

class DrillBook {
public:
    static constexpr int kCap = 64;

    bool set(uint64_t tag, DrillCue cue, int64_t px = 0, int64_t qty = 0) noexcept {
        if (tag == 0 || cue == DrillCue::None) return false;
        for (int i = 0; i < n_; ++i) {
            if (row_[i].tag != tag) continue;
            row_[i].cue = cue;
            row_[i].px = px;
            row_[i].qty = qty;
            return true;
        }
        if (n_ >= kCap) return false;
        row_[n_].tag = tag;
        row_[n_].cue = cue;
        row_[n_].px = px;
        row_[n_].qty = qty;
        ++n_;
        return true;
    }

    bool clear(uint64_t tag) noexcept {
        for (int i = 0; i < n_; ++i) {
            if (row_[i].tag != tag) continue;
            row_[i] = row_[n_ - 1];
            --n_;
            return true;
        }
        return false;
    }

    const DrillRow* find(uint64_t tag) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (row_[i].tag == tag) return &row_[i];
        }
        return nullptr;
    }

    void reset() noexcept { n_ = 0; }
    int size() const noexcept { return n_; }

private:
    DrillRow row_[kCap]{};
    int n_{0};
};

} // namespace pitlane
