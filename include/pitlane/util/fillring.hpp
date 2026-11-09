#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace pitlane {

// One producer claims a slot, fills it in place, then publishes it.
// One consumer copies it out. A full ring or a second claim fails. Nothing is overwritten.
// Each side keeps a local copy of the other side's index and reloads it only when that copy
// says the ring is full or empty.

template <class T, uint32_t Cap>
class FillRing {
public:
    static_assert((Cap & (Cap - 1u)) == 0u, "power of two");
    static_assert(Cap >= 2u && Cap <= 1024u, "slots");
    static_assert(std::is_trivially_copyable<T>::value, "slot");

    T* claim() noexcept {
        if (held_) return nullptr;
        if (tail_ - head_cache_ >= Cap) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail_ - head_cache_ >= Cap) return nullptr;
        }
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
        if (cons_ >= pub_cache_) {
            pub_cache_ = pub_.load(std::memory_order_acquire);
            if (cons_ >= pub_cache_) return false;
        }
        out = slot_[cons_ & (Cap - 1u)];
        ++cons_;
        head_.store(cons_, std::memory_order_release);
        return true;
    }

private:
    T slot_[Cap]{};
    alignas(64) uint64_t tail_{0};
    uint64_t head_cache_{0};
    uint8_t held_{0};
    alignas(64) uint64_t cons_{0};
    uint64_t pub_cache_{0};
    alignas(64) std::atomic<uint64_t> pub_{0};
    alignas(64) std::atomic<uint64_t> head_{0};
};

}  // namespace pitlane
