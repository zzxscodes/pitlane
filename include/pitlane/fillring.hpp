#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace pitlane {

// One producer claims a slot, fills it in place, then publishes it.
// One consumer copies it out. A full ring or a second claim fails. Nothing is overwritten.

template <class T, uint32_t Cap>
class FillRing {
public:
    static_assert((Cap & (Cap - 1u)) == 0u, "power of two");
    static_assert(Cap >= 2u && Cap <= 1024u, "slots");
    static_assert(std::is_trivially_copyable<T>::value, "slot");

    T* claim() noexcept {
        if (held_) return nullptr;
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (tail_ - head >= Cap) return nullptr;
        held_ = 1;
        return &slot_[tail_ & (Cap - 1u)];
    }

    bool publish() noexcept {
        if (!held_) return false;
        held_ = 0;
        ++tail_;
        pub_.store(tail_, std::memory_order_release);
        return true;
    }

    void cancel() noexcept { held_ = 0; }

    bool take(T& out) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t pub = pub_.load(std::memory_order_acquire);
        if (head >= pub) return false;
        out = slot_[head & (Cap - 1u)];
        head_.store(head + 1u, std::memory_order_release);
        return true;
    }

private:
    T slot_[Cap]{};
    uint64_t tail_{0};
    uint8_t held_{0};
    alignas(64) std::atomic<uint64_t> pub_{0};
    alignas(64) std::atomic<uint64_t> head_{0};
};

}  // namespace pitlane
