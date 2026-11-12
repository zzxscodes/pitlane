#pragma once

#include "pitlane/util/hint.hpp"
#include "pitlane/time/cycle.hpp"

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace pitlane {

enum class SlotPri : uint8_t { Normal = 0, High = 1 };
enum class SlotSt : uint8_t { Free = 0, Filling = 1, Ready = 2, Live = 3 };

// Priority triple buffer. High may overwrite Ready/Filling normal slots.
// Single writer. Fail-closed when no slot can be taken.

template <class T, int Slots = 3>
class PrioSnap {
public:
    static_assert(Slots >= 2 && Slots <= 8, "slots");
    static_assert(std::is_trivially_copyable<T>::value, "body");

    const T* view() const noexcept {
        const uint32_t i = live_.load(std::memory_order_acquire);
        if (unlikely(i >= static_cast<uint32_t>(Slots))) return nullptr;
        return &slot_[i];
    }

    PITLANE_HOT bool write(const T& s, SlotPri pri) noexcept {
        const int i = pick(pri);
        if (unlikely(i < 0)) return false;
        st_[i] = SlotSt::Filling;
        slot_[i] = s;
        if (pri == SlotPri::High) flip(static_cast<uint32_t>(i));
        else st_[i] = SlotSt::Ready;
        return true;
    }

    void flush_ready() noexcept {
        for (int i = 0; i < Slots; ++i) {
            if (st_[i] != SlotSt::Ready) continue;
            flip(static_cast<uint32_t>(i));
            return;
        }
    }

private:
    int pick(SlotPri pri) const noexcept {
        for (int i = 0; i < Slots; ++i) {
            if (st_[i] == SlotSt::Free) return i;
        }
        if (pri != SlotPri::High) return -1;
        for (int i = 0; i < Slots; ++i) {
            if (st_[i] == SlotSt::Ready) return i;
        }
        for (int i = 0; i < Slots; ++i) {
            if (st_[i] == SlotSt::Filling) return i;
        }
        return -1;
    }

    void flip(uint32_t i) noexcept {
        st_[i] = SlotSt::Live;
        const uint32_t old = live_.exchange(i, std::memory_order_acq_rel);
        if (old < static_cast<uint32_t>(Slots) && old != i) st_[old] = SlotSt::Free;
    }

    T slot_[Slots]{};
    SlotSt st_[Slots]{};
    std::atomic<uint32_t> live_{Slots};  // none published yet
};

// Multi-reader snapshots with per-slot refcounts. publish fails when every spare
// slot still has a non-zero refcount. copy_out returns 0 on tear after bounded
// retries (fail-closed, no infinite spin).

template <class T, int Slots = 3>
class PinBank {
public:
    static_assert(Slots >= 3 && Slots <= 8, "slots");
    static_assert(std::is_trivially_copyable<T>::value, "body");

    T* write_slot() noexcept { return &slot_[fill_]; }

    PITLANE_HOT bool publish() noexcept {
        int nxt = -1;
        const uint32_t pub = live_.load(std::memory_order_relaxed);
        for (int i = 0; i < Slots; ++i) {
            if (static_cast<uint32_t>(i) == pub || static_cast<uint32_t>(i) == fill_) continue;
            if (pins_[i].load(std::memory_order_relaxed) == 0) {
                nxt = i;
                break;
            }
        }
        if (unlikely(nxt < 0)) return false;
        ++ver_[fill_];
        live_.store(fill_, std::memory_order_release);
        fill_ = static_cast<uint32_t>(nxt);
        return true;
    }

    PITLANE_HOT uint64_t copy_out(T& out) noexcept {
        for (int attempt = 0; attempt < 64; ++attempt) {
            const uint32_t i = live_.load(std::memory_order_acquire);
            if (unlikely(i >= static_cast<uint32_t>(Slots))) return 0;
            pins_[i].fetch_add(1, std::memory_order_acq_rel);
            if (live_.load(std::memory_order_acquire) != i) {
                pins_[i].fetch_sub(1, std::memory_order_release);
                cpu_relax();
                continue;
            }
            out = slot_[i];
            const uint64_t v = ver_[i];
            pins_[i].fetch_sub(1, std::memory_order_release);
            return v == 0 ? 1 : v;
        }
        return 0;
    }

private:
    alignas(kCacheLine) T slot_[Slots]{};
    alignas(kCacheLine) uint64_t ver_[Slots]{};
    alignas(kCacheLine) std::atomic<uint32_t> pins_[Slots]{};
    alignas(kCacheLine) std::atomic<uint32_t> live_{Slots};
    uint32_t fill_{0};
};

}  // namespace pitlane
