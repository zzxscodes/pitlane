#pragma once

#include "pitlane/cycle.hpp"
#include "pitlane/numeric.hpp"

#include <atomic>
#include <cstdint>

namespace pitlane {

// A seqlock page of cycle-to-time factors. One writer publishes. Readers retry while the generation is odd.
// A generation of 0 means the page has never been published. The library does not sample a clock itself.

struct ClockPage {
    std::atomic<uint32_t> gen{0};
    uint64_t sum_ticks{0};
    uint64_t sum_ns{0};
    uint64_t base_tsc{0};
    int64_t base_sec{0};
    int64_t base_nsec{0};
};

struct ClockView {
    uint32_t gen{0};
    uint64_t sum_ticks{0};
    uint64_t sum_ns{1};
    uint64_t base_tsc{0};
    int64_t base_sec{0};
    int64_t base_nsec{0};
    uint64_t cached_stamp{0};
    int64_t cached_sec{0};
    int64_t cached_nsec{0};
    uint8_t has_cache{0};

    bool refresh(const ClockPage& page) noexcept {
        uint32_t g = page.gen.load(std::memory_order_acquire);
        if (g == 0) return false;
        if ((g & 1u) == 0 && g == gen) return true;
        for (int spin = 0; spin < 100000; ++spin) {
            g = page.gen.load(std::memory_order_acquire);
            if (g == 0) return false;
            if (g & 1u) {
                cpu_relax();
                continue;
            }
            const uint64_t ticks = page.sum_ticks;
            const uint64_t ns = page.sum_ns;
            const uint64_t base = page.base_tsc;
            const int64_t sec = page.base_sec;
            const int64_t nsec = page.base_nsec;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (page.gen.load(std::memory_order_relaxed) != g) continue;
            gen = g;
            sum_ticks = ticks;
            sum_ns = ns == 0 ? 1 : ns;
            base_tsc = base;
            base_sec = sec;
            base_nsec = nsec;
            has_cache = 0;
            return true;
        }
        return false;
    }

    bool to_wall(uint64_t stamp, int64_t& sec, int64_t& nsec) noexcept {
        if (sum_ticks == 0) return false;
        if (has_cache && cached_stamp == stamp) {
            sec = cached_sec;
            nsec = cached_nsec;
            return true;
        }
        const bool back = stamp < base_tsc;
        const uint64_t delta = back ? base_tsc - stamp : stamp - base_tsc;
        if (delta > static_cast<uint64_t>(INT64_MAX)) return false;
        const Wide prod = mul_wide(delta, sum_ns);
        uint64_t quot = 0;
        if (!div_wide(prod, sum_ticks, quot)) return false;
        if (quot > static_cast<uint64_t>(INT64_MAX)) return false;
        const int64_t dns = back ? -static_cast<int64_t>(quot) : static_cast<int64_t>(quot);
        if ((dns > 0 && base_nsec > INT64_MAX - dns) || (dns < 0 && base_nsec < INT64_MIN - dns)) return false;
        sec = base_sec;
        nsec = base_nsec + dns;
        constexpr int64_t kNs = 1000000000ll;
        if (nsec >= kNs) {
            const int64_t add = nsec / kNs;
            if (sec > INT64_MAX - add) return false;
            sec += add;
            nsec %= kNs;
        } else if (nsec < 0) {
            const int64_t borrow = (-nsec + kNs - 1) / kNs;
            if (sec < INT64_MIN + borrow) return false;
            sec -= borrow;
            nsec += borrow * kNs;
        }
        cached_stamp = stamp;
        cached_sec = sec;
        cached_nsec = nsec;
        has_cache = 1;
        return true;
    }
};

inline bool clock_publish(ClockPage& page, uint64_t sum_ticks, uint64_t sum_ns, uint64_t base_tsc, int64_t base_sec,
                          int64_t base_nsec) noexcept {
    if (sum_ticks == 0 || sum_ticks > (UINT64_MAX >> 1)) return false;
    uint32_t g = page.gen.load(std::memory_order_relaxed);
    uint32_t odd = g + 1u;
    if (odd == 0) odd = 1u;
    page.gen.store(odd, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    page.sum_ticks = sum_ticks;
    page.sum_ns = sum_ns;
    page.base_tsc = base_tsc;
    page.base_sec = base_sec;
    page.base_nsec = base_nsec;
    std::atomic_thread_fence(std::memory_order_release);
    uint32_t even = odd + 1u;
    if (even == 0) even = 2u;
    page.gen.store(even, std::memory_order_release);
    return true;
}

}  // namespace pitlane
