#pragma once

#include <atomic>
#include <cstdint>

namespace pitlane {

// One command that must not wait behind a queue. A second post fails while the first is still there.
// Tag 0 is empty. The library does not run the command.

struct Urgent {
    std::atomic<uint64_t> slot{0};

    bool post(uint64_t tag) noexcept {
        if (tag == 0) return false;
        uint64_t empty = 0;
        return slot.compare_exchange_strong(empty, tag, std::memory_order_seq_cst, std::memory_order_seq_cst);
    }

    uint64_t take() noexcept { return slot.exchange(0, std::memory_order_seq_cst); }
};

}  // namespace pitlane
