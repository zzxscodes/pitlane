#pragma once

#include "pitlane/util/hint.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pitlane {

// Caller-owned per-core bank. One fixed stride per core. Variables are bump-
// allocated once at start; the hot path only computes addresses. No heap and
// no free on the hot path.

template <uint32_t Cores, uint32_t Stride>
class CoreBank {
public:
    static_assert(Cores >= 1 && Cores <= 256u, "cores");
    static_assert(Stride >= 64u && (Stride % 64u) == 0u, "stride");

    void reset() noexcept {
        cursor_ = 0;
        std::memset(raw_, 0, sizeof raw_);
    }

    // Allocates size bytes at align for every core. Returns the core-0 handle, or null.
    void* alloc(std::size_t size, std::size_t align = 8) noexcept {
        if (unlikely(size == 0 || size > Stride)) return nullptr;
        if (unlikely(align == 0 || align > 64u || (align & (align - 1)) != 0)) return nullptr;
        const std::size_t at = (cursor_ + align - 1) & ~(align - 1);
        if (unlikely(at > Stride || size > Stride - at)) return nullptr;
        cursor_ = at + size;
        void* handle = raw_ + at;
        for (uint32_t c = 0; c < Cores; ++c) {
            std::memset(static_cast<char*>(handle) + static_cast<std::size_t>(c) * Stride, 0, size);
        }
        return handle;
    }

    // Address of a core-0 handle on another core.
    PITLANE_HOT void* at(void* handle, uint32_t core) noexcept {
        if (unlikely(!handle || core >= Cores)) return nullptr;
        auto* h = static_cast<char*>(handle);
        if (unlikely(h < raw_ || h >= raw_ + static_cast<std::ptrdiff_t>(Stride))) return nullptr;
        const std::size_t off = static_cast<std::size_t>(h - raw_);
        return raw_ + static_cast<std::size_t>(core) * Stride + off;
    }

    uint32_t cores() const noexcept { return Cores; }
    uint32_t stride() const noexcept { return Stride; }
    std::size_t used() const noexcept { return cursor_; }

private:
    alignas(64) char raw_[static_cast<std::size_t>(Cores) * Stride]{};
    std::size_t cursor_{0};
};

}  // namespace pitlane
