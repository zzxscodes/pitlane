#pragma once

#include <cstdint>

namespace pitlane {

class WantMask {
public:
    static constexpr uint32_t kCap = 4096;
    void filter(bool on) noexcept { on_ = on ? 1 : 0; }
    void allow(uint32_t id, bool yes) noexcept;
    bool accept(uint32_t id) const noexcept;

private:
    uint8_t bit_[kCap]{};
    uint8_t on_{0};
};

void patch_band(int64_t& up, int64_t& down, int64_t fallback_up, int64_t fallback_down) noexcept;
uint64_t tape_index(int venue, uint64_t order_no, uint64_t seq_no) noexcept;

struct LinkBeat {
    int interval_s{0};
    int misses{0};
    int connect_s{0};
    int mode{0};
};

LinkBeat normalize_beat(LinkBeat in) noexcept;
int64_t scale_px(int64_t raw, int64_t den) noexcept;

struct RawTick {
    int64_t px{0};
    int64_t qty{0};
    int64_t exch_us{0};
    int64_t desk_us{0};
    int64_t recv_us{0};
    char tag{0};
};

// Drops session-switch records (tag 'S') and scales price by den.
int take_ticks(const RawTick* in, int n, int64_t recv_us, int64_t den, RawTick* out, int cap) noexcept;

}  // namespace pitlane
