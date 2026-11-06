#pragma once

#include <atomic>
#include <cstdint>

namespace pitlane {

// A reader reports the epoch after it has dropped shared pointers.
// elapsed returns false while any online reader is still behind. It does not wait.
// The library does not free the caller's memory.

template <int Readers>
class Grace {
public:
    static_assert(Readers >= 1 && Readers <= 64, "readers");

    void online(uint32_t id) noexcept {
        if (id >= static_cast<uint32_t>(Readers)) return;
        const uint64_t epoch = epoch_.load(std::memory_order_acquire);
        slot_[id].seen.store(epoch, std::memory_order_release);
    }

    void offline(uint32_t id) noexcept {
        if (id >= static_cast<uint32_t>(Readers)) return;
        slot_[id].seen.store(0, std::memory_order_release);
    }

    void quiesce(uint32_t id) noexcept {
        if (id >= static_cast<uint32_t>(Readers)) return;
        const uint64_t epoch = epoch_.load(std::memory_order_acquire);
        if (epoch != slot_[id].seen.load(std::memory_order_relaxed)) {
            slot_[id].seen.store(epoch, std::memory_order_release);
        }
    }

    uint64_t bump() noexcept { return epoch_.fetch_add(1, std::memory_order_release) + 1; }

    bool elapsed(uint64_t token) noexcept {
        if (token <= floor_.load(std::memory_order_relaxed)) return true;
        uint64_t least = UINT64_MAX;
        for (int i = 0; i < Readers; ++i) {
            const uint64_t seen = slot_[i].seen.load(std::memory_order_acquire);
            if (seen == 0) continue;
            if (seen < token) return false;
            if (seen < least) least = seen;
        }
        if (least != UINT64_MAX) floor_.store(least, std::memory_order_relaxed);
        return true;
    }

private:
    struct alignas(64) Slot {
        std::atomic<uint64_t> seen{0};
    };

    Slot slot_[Readers]{};
    alignas(64) std::atomic<uint64_t> epoch_{1};
    alignas(64) std::atomic<uint64_t> floor_{0};
};

template <int Cap>
class RetireList {
public:
    static_assert(Cap >= 2 && Cap <= 64, "retire");

    int size() const noexcept { return n_; }

    bool push(uint64_t token, uint32_t user) noexcept {
        if (n_ >= Cap || token == 0) return false;
        item_[n_].token = token;
        item_[n_].user = user;
        ++n_;
        return true;
    }

    template <int Readers>
    int reap(Grace<Readers>& grace, uint32_t* out, int cap) noexcept {
        int due = 0;
        while (due < n_ && grace.elapsed(item_[due].token)) ++due;
        if (due == 0) return 0;
        if (!out || due > cap) return -1;
        for (int i = 0; i < due; ++i) out[i] = item_[i].user;
        const int rest = n_ - due;
        for (int i = 0; i < rest; ++i) item_[i] = item_[due + i];
        n_ = rest;
        return due;
    }

private:
    struct Item {
        uint64_t token{0};
        uint32_t user{0};
    };

    Item item_[Cap]{};
    int n_{0};
};

}  // namespace pitlane
