#pragma once

#include "pitlane/desk/region.hpp"
#include "pitlane/time/cycle.hpp"
#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstdint>
#include <ctime>

namespace pitlane {

// Calibrated TSC time base. Calibrate once at startup, then read rdtsc on the
// hot path and convert to microseconds with an integer scale. The scale is the
// same CycleScale the rest of the library uses, so a caller may publish it to
// global_scale_set or a ClockPage as well.
//
// The base is CLOCK_MONOTONIC_RAW. publish() therefore advances a desk clock in
// the same monotonic domain; a caller that wants a session or wall epoch adds
// the offset once at the publish boundary, never inside the measured section.

struct TscStamp {
    uint64_t tsc{0};
    int64_t us{0};
};

PITLANE_INLINE bool tsc_stamp(TscStamp& out) noexcept {
#if defined(__x86_64__) || defined(__i386__)
    timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) return false;
    uint64_t tsc = 0;
    if (!cycles_ordered(tsc)) return false;
    out.tsc = tsc;
    out.us = static_cast<int64_t>(ts.tv_sec) * 1000000 + static_cast<int64_t>(ts.tv_nsec) / 1000;
    return true;
#else
    (void)out;
    return false;
#endif
}

class TscClock {
public:
    // Samples two TSC / monotonic pairs and derives mult and shift. A longer
    // window lowers quantisation error; 1ms is plenty for a nanosecond scale.
    // Call this on the thread that will read the clock, after pinning it.
    bool calibrate(int window_us = 1000) noexcept {
        on_ = 0;
        TscStamp first{};
        if (unlikely(!tsc_stamp(first))) return false;
        const int want = window_us < 50 ? 50 : (window_us > 100000 ? 100000 : window_us);
        TscStamp last = first;
        for (int i = 0; i < 2000000; ++i) {
            if (unlikely(!tsc_stamp(last))) return false;
            if (last.us - first.us >= want) break;
            cpu_relax();
        }
        if (unlikely(last.tsc <= first.tsc || last.us <= first.us)) return false;
        const uint64_t dcy = last.tsc - first.tsc;
        const uint64_t dns = static_cast<uint64_t>(last.us - first.us) * 1000ull;
        CycleScale next{};
        if (unlikely(!scale_set(next, dcy, dns))) return false;
        scale_ = next;
        base_tsc_ = last.tsc;
        base_ns_ = last.us * 1000ll;
        on_ = 1;
        return true;
    }

    bool ready() const noexcept { return on_ != 0; }
    uint64_t base_tsc() const noexcept { return base_tsc_; }
    int64_t base_ns() const noexcept { return base_ns_; }
    const CycleScale& scale() const noexcept { return scale_; }

    PITLANE_HOT PITLANE_INLINE bool now_ns(uint64_t& out) const noexcept {
        if (unlikely(!on_)) return false;
        uint64_t tsc = 0;
        if (unlikely(!cycles_read(tsc))) return false;
        const bool back = tsc < base_tsc_;
        const uint64_t delta = back ? base_tsc_ - tsc : tsc - base_tsc_;
        uint64_t rel = 0;
        if (unlikely(!scale_ns(scale_, delta, rel))) return false;
        if (back) {
            if (unlikely(rel > static_cast<uint64_t>(INT64_MAX))) return false;
            const int64_t base = base_ns_;
            if (unlikely(static_cast<int64_t>(rel) > base)) return false;
            out = static_cast<uint64_t>(base - static_cast<int64_t>(rel));
            return true;
        }
        const uint64_t base = static_cast<uint64_t>(base_ns_);
        if (unlikely(base > UINT64_MAX - rel)) return false;
        out = base + rel;
        return true;
    }

    PITLANE_HOT PITLANE_INLINE bool now_us(int64_t& out) const noexcept {
        uint64_t ns = 0;
        if (unlikely(!now_ns(ns))) return false;
        out = static_cast<int64_t>(ns / 1000ull);
        return true;
    }

    // Convenience for the common case: 0 means the clock was not calibrated.
    PITLANE_INLINE int64_t now_us() const noexcept {
        int64_t us = 0;
        return now_us(us) ? us : 0;
    }

    // Publishes the current microsecond value to the desk clock. offset_us lets
    // the caller move the monotonic base onto a session epoch in one add.
    PITLANE_INLINE bool publish(Desk& desk, int64_t offset_us = 0) noexcept {
        int64_t us = 0;
        if (unlikely(!now_us(us))) return false;
        if (unlikely(offset_us > 0 && us > INT64_MAX - offset_us)) return false;
        if (unlikely(offset_us < 0 && us < INT64_MIN - offset_us)) return false;
        desk.clock_us.store(us + offset_us, std::memory_order_release);
        return true;
    }

    PITLANE_INLINE bool publish(UsSlot& slot) noexcept {
        int64_t us = 0;
        if (unlikely(!now_us(us))) return false;
        us_store(slot, us);
        return true;
    }

private:
    uint64_t base_tsc_{0};
    int64_t base_ns_{0};
    CycleScale scale_{};
    uint8_t on_{0};
};

} // namespace pitlane
