#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace pitlane {

// Claim a sequence, fill it, then take results in claim order.
// A later sequence that finishes first stays put until the earlier one finishes.
// The ring fails when every slot is still owned. It does not overwrite a slot.

template <class T, uint32_t Cap>
class StageRing {
public:
    static_assert((Cap & (Cap - 1u)) == 0u, "power of two");
    static_assert(Cap >= 4u && Cap <= 1024u, "capacity");
    static_assert(std::is_trivially_copyable<T>::value, "slot");

    bool acquire(uint32_t& seq) noexcept {
        uint32_t h = claim_.load(std::memory_order_relaxed);
        for (;;) {
            const uint32_t done = freed_.load(std::memory_order_acquire);
            if (h - done >= Cap) return false;
            if (claim_.compare_exchange_weak(h, h + 1u, std::memory_order_relaxed, std::memory_order_relaxed)) {
                seq = h;
                return true;
            }
        }
    }

    bool finish(uint32_t seq, uint32_t stage, const T& body) noexcept {
        if (stage > 1u) return false;
        Cell& cell = cell_[seq & kMask];
        if (stage == 0) cell.body = body;
        cell.seq_plus.store(seq + 1u, std::memory_order_relaxed);
        cell.stage.store(stage + 1u, std::memory_order_release);
        return true;
    }

    bool take(uint32_t stage, uint32_t& seq, T& body) noexcept {
        if (stage > 1u) return false;
        seq = cursor_[stage];
        Cell& cell = cell_[seq & kMask];
        if (cell.stage.load(std::memory_order_acquire) != stage + 1u) return false;
        if (cell.seq_plus.load(std::memory_order_relaxed) != seq + 1u) return false;
        body = cell.body;
        cursor_[stage] = seq + 1u;
        if (stage == 1u) freed_.store(cursor_[stage], std::memory_order_release);
        return true;
    }

private:
    static constexpr uint32_t kMask = Cap - 1u;

    struct Cell {
        alignas(64) std::atomic<uint32_t> stage{0};
        std::atomic<uint32_t> seq_plus{0};
        T body{};
    };

    Cell cell_[Cap];
    alignas(64) std::atomic<uint32_t> claim_{0};
    alignas(64) std::atomic<uint32_t> freed_{0};
    uint32_t cursor_[2]{};
};

}  // namespace pitlane
