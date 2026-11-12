#pragma once

#include "pitlane/desk/wire.hpp"

#include <cstdint>

namespace pitlane {

struct PulseSub {
    uint32_t name{0};
    char topic[24]{};
    int64_t from_us{0};
    uint8_t dense_only{0};
    uint8_t live{0};
};

class PulseBoard {
public:
    static constexpr int kSubs = 128;
    static constexpr int kQueue = 256;

    bool watch(uint32_t name, const char* topic, int64_t from_us, bool dense_only) noexcept;
    void clear() noexcept;
    bool offer(const Pulse& pulse) noexcept;
    int drain(Pulse* out, int cap) noexcept;
    int queued() const noexcept { return count_; }
    int subscribers() const noexcept { return nsub_; }

    // Snapshot live subscriptions before a fan-out send (notes §40).
    int copy_subs(PulseSub* dst, int cap) const noexcept;

private:
    using Sub = PulseSub;
    Sub sub_[kSubs]{};
    Pulse q_[kQueue]{};
    int head_{0};
    int count_{0};
    int nsub_{0};
};

}  // namespace pitlane
