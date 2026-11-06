#pragma once

#include <cstdint>

namespace pitlane {

// Fixed timestamps, a half round-trip offset, and log-sized buckets.
// The caller supplies every tick. The library does not read a hardware counter.

template <int N>
struct LatTrace {
    static_assert(N >= 2 && N <= 16, "nodes");
    int64_t ts[N]{};
    uint8_t set_[N]{};
};

template <int N>
inline bool lat_mark(LatTrace<N>& trace, int node, int64_t tick, bool enabled) noexcept {
    if (node < 0 || node >= N || tick < 0) return false;
    if (!enabled) return true;
    trace.ts[node] = tick;
    trace.set_[node] = 1;
    return true;
}

template <int N>
inline bool lat_span(const LatTrace<N>& trace, int a, int b, int64_t& out) noexcept {
    if (a < 0 || b < 0 || a >= N || b >= N) return false;
    if (!trace.set_[a] || !trace.set_[b]) return false;
    out = trace.ts[b] - trace.ts[a];
    return true;
}

struct DeskSkew {
    int64_t skew{0};
    uint8_t on{0};
};

inline bool skew_sample(DeskSkew& clock, int64_t local_ms, int64_t desk_ms) noexcept {
    if ((local_ms > 0 && desk_ms < INT64_MIN + local_ms) || (local_ms < 0 && desk_ms > INT64_MAX + local_ms)) return false;
    clock.skew = (desk_ms - local_ms) / 2;
    clock.on = 1;
    return true;
}

inline bool skew_apply(const DeskSkew& clock, int64_t local_ms, int64_t& out) noexcept {
    if (!clock.on) return false;
    if ((clock.skew > 0 && local_ms > INT64_MAX - clock.skew) || (clock.skew < 0 && local_ms < INT64_MIN - clock.skew)) {
        return false;
    }
    out = local_ms + clock.skew;
    return true;
}

template <int Buckets>
class LatHist {
public:
    static_assert(Buckets >= 4 && Buckets <= 32, "buckets");

    bool add(int64_t dt) noexcept {
        if (dt < 0) return false;
        int bucket = 0;
        if (dt > 1) {
            uint64_t v = static_cast<uint64_t>(dt);
            while (v > 1 && bucket < Buckets - 1) {
                v >>= 1;
                ++bucket;
            }
        }
        if (n_[bucket] == UINT64_MAX) return false;
        ++n_[bucket];
        return true;
    }

    uint64_t at(int bucket) const noexcept {
        if (bucket < 0 || bucket >= Buckets) return 0;
        return n_[bucket];
    }

    uint64_t cum(int bucket) const noexcept {
        if (bucket < 0) return 0;
        if (bucket >= Buckets) bucket = Buckets - 1;
        uint64_t sum = 0;
        for (int i = 0; i <= bucket; ++i) {
            if (sum > UINT64_MAX - n_[i]) return UINT64_MAX;
            sum += n_[i];
        }
        return sum;
    }

private:
    uint64_t n_[Buckets]{};
};

}  // namespace pitlane
