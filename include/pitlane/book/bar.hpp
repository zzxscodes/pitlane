#pragma once

#include <cstdint>

namespace pitlane {

// Builds one bar from ticks the caller already accepts as tradable.
// A tick outside the session is ignored. Empty periods are not invented.

struct Bar {
    int64_t start_us{0};
    int64_t open{0};
    int64_t high{0};
    int64_t low{0};
    int64_t close{0};
    int64_t vol{0};
    int64_t amt{0};
    uint8_t on{0};
};

class BarFold {
public:
    explicit BarFold(int64_t span_us) noexcept : span_(span_us) {}

    bool on_tick(int64_t ts_us, int64_t px, int64_t qty, bool tradable, Bar* closed) noexcept {
        if (closed) *closed = Bar{};
        if (!tradable || span_ <= 0 || ts_us < 0 || px <= 0 || qty < 0) return false;
        const int64_t bucket = ts_us - (ts_us % span_);
        if (cur_.on && bucket != cur_.start_us) {
            if (bucket < cur_.start_us) return false;
            if (qty > 0 && px > INT64_MAX / qty) return false;
            if (closed) *closed = cur_;
            cur_ = Bar{};
            return open_at(bucket, px, qty);
        }
        if (!cur_.on) return open_at(bucket, px, qty);
        return apply(px, qty);
    }

    bool seal(Bar* closed) noexcept {
        if (!cur_.on) return false;
        if (closed) *closed = cur_;
        cur_ = Bar{};
        return true;
    }

    const Bar& current() const noexcept { return cur_; }

private:
    bool open_at(int64_t bucket, int64_t px, int64_t qty) noexcept {
        if (qty > 0 && px > INT64_MAX / qty) return false;
        cur_.start_us = bucket;
        cur_.open = px;
        cur_.high = px;
        cur_.low = px;
        cur_.close = px;
        cur_.vol = qty;
        cur_.amt = qty > 0 ? px * qty : 0;
        cur_.on = 1;
        return true;
    }

    bool apply(int64_t px, int64_t qty) noexcept {
        if (qty > 0 && (px > INT64_MAX / qty || cur_.vol > INT64_MAX - qty)) return false;
        const int64_t amt = qty > 0 ? px * qty : 0;
        if (cur_.amt > INT64_MAX - amt) return false;
        if (px > cur_.high) cur_.high = px;
        if (px < cur_.low) cur_.low = px;
        cur_.close = px;
        cur_.vol += qty;
        cur_.amt += amt;
        return true;
    }

    int64_t span_{0};
    Bar cur_{};
};

}  // namespace pitlane
