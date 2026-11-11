#pragma once

#include "pitlane/time/cycle.hpp"
#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstdint>

namespace pitlane {

// Optional process-wide integer cycle scale. Hot paths load once per conversion.
// mult and shift sit on separate lines from the generation so a writer does not
// bounce the reader's scale line while only bumping the generation.
// The library still accepts a caller-owned CycleScale. This is the shared path.

namespace detail {

struct alignas(kCacheLine) GlobalScaleWords {
    std::atomic<uint64_t> mult{0};
    std::atomic<uint32_t> shift{0};
    std::atomic<uint32_t> gen{0};
    std::atomic<uint8_t> on{0};
};

inline GlobalScaleWords g_scale_storage{};

inline GlobalScaleWords& global_scale_words() noexcept { return g_scale_storage; }

}  // namespace detail

PITLANE_INLINE bool global_scale_set(uint64_t cycles, uint64_t ns) noexcept {
    CycleScale local{};
    if (!scale_set(local, cycles, ns)) return false;
    auto& g = detail::global_scale_words();
    g.on.store(0, std::memory_order_relaxed);
    g.mult.store(local.mult, std::memory_order_relaxed);
    g.shift.store(local.shift, std::memory_order_relaxed);
    g.gen.fetch_add(1, std::memory_order_acq_rel);
    g.on.store(1, std::memory_order_release);
    return true;
}

PITLANE_INLINE void global_scale_clear() noexcept {
    auto& g = detail::global_scale_words();
    g.on.store(0, std::memory_order_release);
}

PITLANE_HOT PITLANE_INLINE bool global_scale_ns(uint64_t cycles, uint64_t& ns) noexcept {
    const auto& g = detail::global_scale_words();
    if (unlikely(g.on.load(std::memory_order_acquire) == 0)) return false;
    CycleScale local{};
    local.mult = g.mult.load(std::memory_order_relaxed);
    local.shift = g.shift.load(std::memory_order_relaxed);
    local.on = 1;
    return scale_ns(local, cycles, ns);
}

PITLANE_INLINE bool global_scale_on() noexcept {
    return detail::global_scale_words().on.load(std::memory_order_acquire) != 0;
}

PITLANE_INLINE uint32_t global_scale_gen() noexcept {
    return detail::global_scale_words().gen.load(std::memory_order_acquire);
}

}  // namespace pitlane
