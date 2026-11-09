#pragma once

#include "pitlane/util/numeric.hpp"

#include <atomic>
#include <cstdint>

namespace pitlane {

// Cycle reads, an integer scale, and a cached microsecond slot.
// The caller owns every object. The library does not start a thread,
// does not open a socket, and does not call a clock in the measured section.

inline void compiler_fence() noexcept { asm volatile("" ::: "memory"); }

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");
#else
    compiler_fence();
#endif
}

inline void load_fence() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    asm volatile("lfence" ::: "memory");
#else
    compiler_fence();
#endif
}

template <class T>
inline void keep(const T& value) noexcept {
    asm volatile("" : : "m"(value) : "memory");
}

inline bool cycles_read(uint64_t& out) noexcept {
#if defined(__x86_64__) || defined(__i386__)
    unsigned lo = 0;
    unsigned hi = 0;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    out = (static_cast<uint64_t>(hi) << 32) | lo;
    return true;
#else
    out = 0;
    return false;
#endif
}

inline bool cycles_ordered(uint64_t& out) noexcept {
#if defined(__x86_64__) || defined(__i386__)
    unsigned lo = 0;
    unsigned hi = 0;
    unsigned aux = 0;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    (void)aux;
    out = (static_cast<uint64_t>(hi) << 32) | lo;
    return true;
#else
    out = 0;
    return false;
#endif
}

inline bool cycle_gap(uint64_t begin, uint64_t end, uint64_t& out) noexcept {
    if (end < begin) return false;
    out = end - begin;
    return true;
}

// One cache line. The caller writes it. Hot path code only loads it.
struct alignas(64) UsSlot {
    std::atomic<int64_t> us{0};
    unsigned char pad_[64 - sizeof(std::atomic<int64_t>)]{};
};

static_assert(sizeof(UsSlot) == 64, "microsecond slot occupies one line");

inline void us_store(UsSlot& slot, int64_t us) noexcept { slot.us.store(us, std::memory_order_release); }

inline int64_t us_load(const UsSlot& slot) noexcept { return slot.us.load(std::memory_order_acquire); }

// Nanoseconds are (cycles * mult + half) >> shift. The caller supplies the pair.
struct CycleScale {
    uint64_t mult{0};
    uint32_t shift{0};
    uint8_t on{0};
};

inline bool scale_set(CycleScale& scale, uint64_t cycles, uint64_t ns) noexcept {
    if (cycles == 0 || ns == 0 || cycles > (UINT64_MAX >> 1)) return false;
    for (uint32_t shift = 32; shift >= 1; --shift) {
        Wide wide;
        wide.hi = ns >> (64u - shift);
        wide.lo = ns << shift;
        uint64_t mult = 0;
        if (!div_wide(wide, cycles, mult) || mult == 0) continue;
        scale.mult = mult;
        scale.shift = shift;
        scale.on = 1;
        return true;
    }
    scale.on = 0;
    return false;
}

inline bool scale_ns(const CycleScale& scale, uint64_t cycles, uint64_t& ns) noexcept {
    if (!scale.on || scale.shift == 0 || scale.shift >= 64) return false;
    if (cycles == 0) {
        ns = 0;
        return true;
    }
    Wide prod = mul_wide(cycles, scale.mult);
    const uint64_t half = 1ull << (scale.shift - 1u);
    const uint64_t lo = prod.lo + half;
    prod.hi += lo < prod.lo ? 1ull : 0ull;
    prod.lo = lo;
    if ((prod.hi >> scale.shift) != 0) return false;
    ns = (prod.lo >> scale.shift) | (prod.hi << (64u - scale.shift));
    return true;
}

inline bool scale_span(const CycleScale& scale, uint64_t begin, uint64_t end, uint64_t& ns) noexcept {
    uint64_t delta = 0;
    if (!cycle_gap(begin, end, delta)) return false;
    return scale_ns(scale, delta, ns);
}

// Warmup samples are discarded. The caller runs the measured code between begin and end.
struct CycleProbe {
    uint64_t t0{0};
    int left{0};
    uint8_t open{0};

    void arm(int warmup) noexcept {
        left = warmup > 0 ? warmup : 0;
        open = 0;
    }

    bool begin() noexcept {
        load_fence();
        if (!cycles_read(t0)) return false;
        open = 1;
        return true;
    }

    // 1: keep the sample. 0: warmup, discarded. -1: the read failed or went backwards.
    int end(uint64_t& cycles) noexcept {
        uint64_t t1 = 0;
        if (!open || !cycles_ordered(t1)) return -1;
        load_fence();
        open = 0;
        if (t1 < t0) return -1;
        cycles = t1 - t0;
        if (left > 0) {
            --left;
            return 0;
        }
        return 1;
    }
};

enum class Duty : int8_t { Skip = 0, Fast = 1, Slow = 2, Timeout = 3 };

// Decides whether this lap of the caller's loop should run. It does not poll a descriptor.
struct DutyPace {
    uint64_t last_fast{0};
    uint64_t last_slow{0};
    uint64_t fast_gap{0};
    uint64_t slow_gap{0};
    uint64_t per_ms{1};
    uint8_t fast_set{0};
    uint8_t slow_set{0};

    bool open(uint64_t fast_cycles, uint64_t slow_cycles, uint64_t cycles_per_ms) noexcept {
        if (fast_cycles == 0 || slow_cycles < fast_cycles || cycles_per_ms == 0) return false;
        fast_gap = fast_cycles;
        slow_gap = slow_cycles;
        per_ms = cycles_per_ms;
        fast_set = 0;
        slow_set = 0;
        return true;
    }

    Duty step(uint64_t now, int timeout_ms, uint64_t start, bool blocking) noexcept {
        if (!blocking) {
            if (fast_set && now - last_fast < fast_gap) return Duty::Skip;
            last_fast = now;
            fast_set = 1;
            return Duty::Fast;
        }
        if (timeout_ms >= 0) {
            const uint64_t ms = static_cast<uint64_t>(timeout_ms);
            if (ms != 0 && per_ms > UINT64_MAX / ms) return Duty::Timeout;
            if (now - start >= ms * per_ms) return Duty::Timeout;
        }
        if (!slow_set || now - last_slow >= slow_gap) {
            last_slow = now;
            last_fast = now;
            slow_set = 1;
            fast_set = 1;
            return Duty::Slow;
        }
        last_fast = now;
        fast_set = 1;
        return Duty::Fast;
    }
};

inline bool deadline_at(uint64_t origin, uint64_t offset, uint64_t& out) noexcept {
    if (offset > UINT64_MAX - origin) return false;
    out = origin + offset;
    return true;
}

inline bool before_deadline(uint64_t now, uint64_t deadline) noexcept {
    return static_cast<int64_t>(deadline - now) > 0;
}

}  // namespace pitlane
