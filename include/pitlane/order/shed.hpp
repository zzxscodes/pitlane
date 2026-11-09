#pragma once

#include <cstdint>

namespace pitlane {

// Drops a queued item only after it has stayed too long for a whole window.
// The first late item starts the window and is kept. This does not limit order rate.
class StayShed {
public:
    StayShed(int64_t target, int64_t window) noexcept : target_(target), window_(window) {}

    bool drop(int64_t enqueued, int64_t now) noexcept {
        if (target_ <= 0 || window_ <= 0) return false;
        const int64_t stay = now >= enqueued ? now - enqueued : 0;
        if (stay < target_) {
            dropping_ = false;
            first_above_ = 0;
            return false;
        }
        if (first_above_ == 0) {
            if (now > INT64_MAX - window_) return false;
            first_above_ = now + window_;
            return false;
        }
        if (now >= first_above_) {
            dropping_ = true;
            return true;
        }
        return dropping_;
    }

    bool dropping() const noexcept { return dropping_; }

private:
    int64_t target_{0};
    int64_t window_{0};
    int64_t first_above_{0};
    bool dropping_{false};
};

}  // namespace pitlane
