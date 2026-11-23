#pragma once

#include <atomic>
#include <cstdint>

namespace pitlane {

// One writer fills a slot the reader is not holding, then publishes it.
// Two slots fail when both are busy. Three slots may drop an unread middle copy.
// The library does not start a thread.

template <typename T, int Slots>
class SnapPub {
public:
    static_assert(Slots == 2 || Slots == 3, "slots");

    T* begin_write() noexcept {
        if (writing_ != kNone) return nullptr;
        const uint32_t pub = published_.load(std::memory_order_acquire);
        const uint32_t hold = held_.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < static_cast<uint32_t>(Slots); ++i) {
            if (i == pub || i == hold) continue;
            writing_ = i;
            return &slot_[i];
        }
        return nullptr;
    }

    bool publish() noexcept {
        if (writing_ == kNone) return false;
        if (held_.load(std::memory_order_acquire) == writing_) return false;
        published_.store(writing_, std::memory_order_release);
        writing_ = kNone;
        return true;
    }

    void cancel_write() noexcept { writing_ = kNone; }

    const T* pin() noexcept {
        const uint32_t pub = published_.load(std::memory_order_acquire);
        if (pub >= static_cast<uint32_t>(Slots)) return nullptr;
        uint32_t expect = kNone;
        if (!held_.compare_exchange_strong(expect, pub, std::memory_order_acq_rel)) return nullptr;
        const uint32_t again = published_.load(std::memory_order_acquire);
        if (again != pub) {
            held_.store(kNone, std::memory_order_release);
            return nullptr;
        }
        return &slot_[pub];
    }

    void unpin() noexcept { held_.store(kNone, std::memory_order_release); }

private:
    static constexpr uint32_t kNone = 0xffffffffu;

    T slot_[Slots]{};
    std::atomic<uint32_t> published_{kNone};
    std::atomic<uint32_t> held_{kNone};
    uint32_t writing_{kNone};
};

}  // namespace pitlane
