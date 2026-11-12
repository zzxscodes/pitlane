#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <type_traits>

namespace pitlane {

// Thread-private fixed freelist. Capacity is compile-time. A full pool fails
// allocate and leaves prior slots unchanged. No heap on the hot path.

template <typename T, uint32_t Cap>
class ObjectPool {
public:
    static_assert(Cap >= 1 && Cap <= 65536u, "cap");
    static_assert(std::is_trivially_copyable<T>::value, "body");

    ObjectPool() noexcept { reset(); }

    void reset() noexcept {
        for (uint32_t i = 0; i + 1 < Cap; ++i) next_[i] = i + 1;
        next_[Cap - 1] = kNil;
        head_ = 0;
        live_ = 0;
    }

    PITLANE_HOT T* allocate() noexcept {
        if (unlikely(head_ == kNil)) return nullptr;
        const uint32_t ix = head_;
        head_ = next_[ix];
        next_[ix] = kNil;
        ++live_;
        return &slots_[ix];
    }

    PITLANE_HOT bool release(T* p) noexcept {
        if (unlikely(!p)) return false;
        const auto* base = reinterpret_cast<const unsigned char*>(&slots_[0]);
        const auto* cur = reinterpret_cast<const unsigned char*>(p);
        if (unlikely(cur < base)) return false;
        const std::size_t off = static_cast<std::size_t>(cur - base);
        if (unlikely(off % sizeof(T) != 0)) return false;
        const uint32_t ix = static_cast<uint32_t>(off / sizeof(T));
        if (unlikely(ix >= Cap || next_[ix] != kNil || live_ == 0)) return false;
        next_[ix] = head_;
        head_ = ix;
        --live_;
        return true;
    }

    uint32_t live() const noexcept { return live_; }
    uint32_t capacity() const noexcept { return Cap; }
    uint32_t free() const noexcept { return Cap - live_; }

    int index_of(const T* p) const noexcept {
        if (!p) return -1;
        const auto* base = reinterpret_cast<const unsigned char*>(&slots_[0]);
        const auto* cur = reinterpret_cast<const unsigned char*>(p);
        if (cur < base) return -1;
        const std::size_t off = static_cast<std::size_t>(cur - base);
        if (off % sizeof(T) != 0) return -1;
        const uint32_t ix = static_cast<uint32_t>(off / sizeof(T));
        return ix < Cap ? static_cast<int>(ix) : -1;
    }

    T* at(uint32_t ix) noexcept { return ix < Cap ? &slots_[ix] : nullptr; }
    const T* at(uint32_t ix) const noexcept { return ix < Cap ? &slots_[ix] : nullptr; }

private:
    static constexpr uint32_t kNil = 0xffffffffu;
    T slots_[Cap]{};
    uint32_t next_[Cap]{};
    uint32_t head_{0};
    uint32_t live_{0};
};

}  // namespace pitlane
