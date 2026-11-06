#include "pitlane/feed.hpp"

namespace pitlane {

void WantMask::allow(uint32_t id, bool yes) noexcept {
    if (id >= kCap) return;
    bit_[id] = yes ? 1 : 0;
}

bool WantMask::accept(uint32_t id) const noexcept {
    if (!on_) return true;
    if (id >= kCap) return false;
    return bit_[id] != 0;
}

void patch_band(int64_t& up, int64_t& down, int64_t fallback_up, int64_t fallback_down) noexcept {
    if (up <= 0) up = fallback_up;
    if (down <= 0) down = fallback_down;
}

uint64_t tape_index(int venue, uint64_t order_no, uint64_t seq_no) noexcept {
    return venue == 0 ? order_no : seq_no;
}

LinkBeat normalize_beat(LinkBeat in) noexcept {
    if (in.interval_s <= 0) in.interval_s = 10;
    if (in.misses <= 0) in.misses = 2;
    if (in.connect_s <= 0) in.connect_s = 30;
    if (in.mode < 0 || in.mode > 2) in.mode = 0;
    return in;
}

int64_t scale_px(int64_t raw, int64_t den) noexcept {
    if (den <= 0) return raw;
    return raw / den;
}

int take_ticks(const RawTick* in, int n, int64_t recv_us, int64_t den, RawTick* out, int cap) noexcept {
    if (!in || !out || n <= 0 || cap <= 0) return 0;
    int w = 0;
    for (int i = 0; i < n && w < cap; ++i) {
        if (in[i].tag == 'S') continue;
        out[w] = in[i];
        out[w].px = scale_px(in[i].px, den);
        out[w].recv_us = recv_us;
        ++w;
    }
    return w;
}

}  // namespace pitlane
