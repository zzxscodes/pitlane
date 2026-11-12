#pragma once

#include <atomic>
#include <cstdint>

namespace pitlane {

// Two-phase ledger freeze (notes §26). Compute first; then first-writer CAS; then commit.
// The library does not send. Fail-closed: a lost race leaves the caller's intent unsent.

struct FreezeSlot {
    std::atomic<uint8_t> state{0};  // 0 empty, 1 reserved, 2 committed
    int64_t px{0};
    int64_t qty{0};
    uint64_t tag{0};
};

inline bool prelock_ok(int64_t free_amt, int64_t notional, int64_t fee, int64_t px, int64_t qty) noexcept {
    if (px <= 0 || qty <= 0 || notional < 0 || fee < 0) return false;
    if (notional > INT64_MAX - fee) return false;
    return free_amt >= notional + fee;
}

// First writer wins. Returns false when the slot is busy or inputs are bad.
inline bool prelock_reserve(FreezeSlot& slot, uint64_t tag, int64_t px, int64_t qty) noexcept {
    if (tag == 0 || px <= 0 || qty <= 0) return false;
    uint8_t expect = 0;
    if (!slot.state.compare_exchange_strong(expect, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }
    slot.tag = tag;
    slot.px = px;
    slot.qty = qty;
    return true;
}

inline bool prelock_commit(FreezeSlot& slot, uint64_t tag) noexcept {
    if (slot.state.load(std::memory_order_acquire) != 1 || slot.tag != tag) return false;
    slot.state.store(2, std::memory_order_release);
    return true;
}

inline bool prelock_abort(FreezeSlot& slot, uint64_t tag) noexcept {
    if (slot.state.load(std::memory_order_acquire) != 1 || slot.tag != tag) return false;
    slot.px = 0;
    slot.qty = 0;
    slot.tag = 0;
    slot.state.store(0, std::memory_order_release);
    return true;
}

inline bool prelock_clear(FreezeSlot& slot, uint64_t tag) noexcept {
    if (slot.state.load(std::memory_order_acquire) != 2 || slot.tag != tag) return false;
    slot.px = 0;
    slot.qty = 0;
    slot.tag = 0;
    slot.state.store(0, std::memory_order_release);
    return true;
}

}  // namespace pitlane
