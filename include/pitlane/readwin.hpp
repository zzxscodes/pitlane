#pragma once

#include <cstdint>

namespace pitlane {

// The write limit is the slowest joined reader plus the window.
// A reader that is not sticky and stays quieter than the hold leaves the set.
// No joined reader means the limit is not granted. The library does not write the bytes.

struct ReadWin {
    static constexpr int kSlots = 8;

    bool open(uint64_t window) noexcept {
        if (window < 4) return false;
        window_ = window;
        for (int i = 0; i < kSlots; ++i) {
            in_[i] = 0;
            sticky_[i] = 0;
            pos_[i] = 0;
            quiet_[i] = 0;
            quiet_on_[i] = 0;
        }
        return true;
    }

    bool join(int slot, uint64_t at, bool sticky) noexcept {
        if (slot < 0 || slot >= kSlots || window_ < 4) return false;
        pos_[slot] = at;
        quiet_[slot] = 0;
        quiet_on_[slot] = 0;
        in_[slot] = 1;
        sticky_[slot] = sticky ? 1 : 0;
        return true;
    }

    bool report(int slot, uint64_t at) noexcept {
        if (slot < 0 || slot >= kSlots || !in_[slot]) return false;
        pos_[slot] = at;
        quiet_[slot] = 0;
        quiet_on_[slot] = 0;
        return true;
    }

    bool limit(uint64_t published, uint64_t now, uint64_t hold, uint64_t& out) noexcept {
        if (window_ < 4) return false;
        uint64_t slow = 0;
        bool any = false;
        for (int i = 0; i < kSlots; ++i) {
            if (!in_[i]) continue;
            if (!sticky_[i]) {
                const uint64_t lag = published - pos_[i];
                if (lag > (window_ >> 2)) {
                    if (!quiet_on_[i]) {
                        quiet_[i] = now;
                        quiet_on_[i] = 1;
                    }
                    if (now - quiet_[i] >= hold) {
                        in_[i] = 0;
                        quiet_on_[i] = 0;
                        continue;
                    }
                } else {
                    quiet_[i] = 0;
                    quiet_on_[i] = 0;
                }
            }
            if (!any || pos_[i] < slow) slow = pos_[i];
            any = true;
        }
        if (!any || slow > UINT64_MAX - window_) return false;
        out = slow + window_;
        return true;
    }

    bool joined(int slot) const noexcept { return slot >= 0 && slot < kSlots && in_[slot] != 0; }

private:
    uint64_t pos_[kSlots]{};
    uint64_t quiet_[kSlots]{};
    uint64_t window_{0};
    uint8_t in_[kSlots]{};
    uint8_t sticky_[kSlots]{};
    uint8_t quiet_on_[kSlots]{};
};

}  // namespace pitlane
