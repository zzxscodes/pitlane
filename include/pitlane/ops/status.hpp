#pragma once

// Read-only status snapshot for the ops console. One writer (the trading
// thread) publishes a fixed-size record; any number of readers copy it out
// under a seqlock. The record holds no pointers and is safe to JSON-encode.

#include <atomic>
#include <cstdint>

namespace pitlane {

struct StatusPosition {
    uint32_t name;
    int64_t today;      // long qty - short qty
    int64_t yday;       // yesterday long - short
    int64_t avg_px;     // average price of the open side
    int64_t realized;
    int64_t unrealized;
};

inline constexpr int kStatusPositions = 8;

struct StatusSnapshot {
    uint64_t seq{0};
    int64_t stamp_us{0};
    uint8_t state{0};  // 0 init, 1 running, 2 paused, 3 halted, 4 stopping
    uint8_t md_up{0};
    uint8_t td_up{0};
    uint8_t strategy_on{0};
    uint8_t halted{0};
    uint32_t config_version{0};
    int64_t last_tick_us{0};
    int64_t uptime_us{0};

    int64_t quotes{0};
    int64_t signals{0};
    int64_t parents{0};
    int64_t child_orders{0};
    int64_t fills{0};
    int64_t rejects{0};
    int64_t cancels{0};

    int64_t net_qty{0};
    int64_t realized{0};
    int64_t unrealized{0};
    int64_t equity{0};
    int64_t peak_equity{0};
    int64_t drawdown{0};
    int64_t turnover{0};
    int64_t fees{0};
    int64_t order_rate_1s{0};
    int64_t reject_rate_ppm{0};

    int32_t pos_n{0};
    StatusPosition pos[kStatusPositions]{};

    int32_t cpu_ppm{0};
    int32_t mem_ppm{0};
    int32_t load_1m_ppm{0};
    int32_t threads{0};

    char note[64]{};
};

static_assert(sizeof(StatusSnapshot) < 1024, "status snapshot stays cache sized");

// Seqlock: publish writes an odd sequence, then the body, then the next even
// sequence. Readers copy and confirm the sequence did not move.
template <int Cap = 4>
class StatusBoard {
public:
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "status board capacity");

    void publish(const StatusSnapshot& s) noexcept {
        const uint64_t v = seq_.load(std::memory_order_relaxed);
        seq_.store(v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        snap_ = s;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(v + 2, std::memory_order_release);
    }

    bool read(StatusSnapshot& out) const noexcept {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const uint64_t before = seq_.load(std::memory_order_acquire);
            if ((before & 1u) != 0) continue;
            out = snap_;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == before) return true;
        }
        return false;
    }

    uint64_t version() const noexcept { return seq_.load(std::memory_order_acquire); }

private:
    alignas(64) std::atomic<uint64_t> seq_{0};
    alignas(64) StatusSnapshot snap_{};
};

}  // namespace pitlane
