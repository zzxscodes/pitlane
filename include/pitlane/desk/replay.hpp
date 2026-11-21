#pragma once

#include "pitlane/desk/ring.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

// Deterministic replay of a captured ring sequence, with explicit fault
// injection. The tape is a bounded, caller-owned array; a full tape fails
// instead of overwriting evidence. FaultPlan is deterministic: the same plan
// and the same tape produce the same run, including the first blocked write.

enum class FaultPoint : uint8_t {
    Publish = 0,
    Ack = 1,
    Capture = 2,
    kCount = 3
};

struct FaultPlan {
    uint32_t points{0}; // bit mask of FaultPoint
    uint32_t skip{0};   // let this many matching actions pass first
    uint32_t at{0};     // 1-based matching action to fail; 0 means use every
    uint32_t every{0};  // fail every Nth matching action; 0 means first after skip
    uint64_t seen[static_cast<int>(FaultPoint::kCount)]{};
    uint64_t hits{0};
};

inline bool fault_take(FaultPlan& plan, FaultPoint point) noexcept {
    const int i = static_cast<int>(point);
    if (i < 0 || i >= static_cast<int>(FaultPoint::kCount)) return false;
    if ((plan.points & (1u << i)) == 0u) return false;
    const uint64_t n = ++plan.seen[i];
    if (plan.skip >= n) return false;
    if (plan.at != 0 && n != plan.at) return false;
    if (plan.at == 0 && plan.every != 0 && (n % plan.every) != 0) return false;
    if (plan.at == 0 && plan.every == 0 && n != 1) return false;
    ++plan.hits;
    return true;
}

inline void fault_reset(FaultPlan& plan) noexcept {
    for (int i = 0; i < static_cast<int>(FaultPoint::kCount); ++i) plan.seen[i] = 0;
    plan.hits = 0;
}

template <class T>
struct ReplayNote {
    uint64_t pos{0};
    uint64_t stamp_us{0};
    T item{};
};

template <class T, uint32_t Cap>
class ReplayTape {
public:
    static_assert((Cap & (Cap - 1u)) == 0u && Cap >= 2u, "replay cap");

    bool push(uint64_t pos, const T& item, uint64_t stamp_us = 0) noexcept {
        if (unlikely(n_ >= Cap)) return false;
        notes_[n_].pos = pos;
        notes_[n_].stamp_us = stamp_us;
        notes_[n_].item = item;
        ++n_;
        return true;
    }

    uint32_t size() const noexcept { return n_; }
    bool full() const noexcept { return n_ >= Cap; }
    void clear() noexcept { n_ = 0; }

    bool at(uint32_t i, ReplayNote<T>& out) const noexcept {
        if (i >= n_) return false;
        out = notes_[i];
        return true;
    }

private:
    ReplayNote<T> notes_[Cap]{};
    uint32_t n_{0};
};

template <class T, uint32_t RingCap, uint32_t TapeCap>
bool replay_capture(ReplayTape<T, TapeCap>& tape, const FanRing<T, RingCap>& ring, uint64_t pos,
                    uint64_t stamp_us = 0) noexcept {
    const T* body = ring.view(pos, 0, nullptr);
    if (unlikely(!body)) return false;
    return tape.push(pos, *body, stamp_us);
}

// Copies every published slot in [from, ring.tail()) into the tape.
template <class T, uint32_t RingCap, uint32_t TapeCap>
uint32_t replay_capture_all(ReplayTape<T, TapeCap>& tape, const FanRing<T, RingCap>& ring, uint64_t from = 0) noexcept {
    uint32_t n = 0;
    const uint64_t end = ring.tail();
    for (uint64_t pos = from; pos < end; ++pos) {
        if (!replay_capture(tape, ring, pos)) break;
        ++n;
    }
    return n;
}

struct ReplayOut {
    uint32_t published{0};
    uint32_t dropped{0};
    uint32_t unacked{0};
    uint32_t blocked{0};
    uint32_t failed{0};
};

// Re-publishes a tape into a ring. FaultPoint::Publish drops an item;
// FaultPoint::Ack leaves the published slot unacked, so a finite ring
// eventually fails the next write instead of overwriting live data. That is
// the fail-closed path a chaos run needs to assert.
template <class T, uint32_t RingCap, uint32_t TapeCap, class Fn>
ReplayOut replay_ring(FanRing<T, RingCap>& ring, const ReplayTape<T, TapeCap>& tape, FaultPlan& plan,
                      Fn&& fn) noexcept {
    ReplayOut out{};
    for (uint32_t i = 0; i < tape.size(); ++i) {
        ReplayNote<T> note{};
        if (unlikely(!tape.at(i, note))) {
            ++out.failed;
            continue;
        }
        if (fault_take(plan, FaultPoint::Publish)) {
            ++out.dropped;
            continue;
        }
        constexpr uint64_t kReaderBit = 1ull; // logical reader 0
        if (unlikely(!ring.publish(note.item, kReaderBit))) {
            ++out.blocked;
            break;
        }
        const uint64_t pos = ring.tail() - 1u;
        ++out.published;
        if (fault_take(plan, FaultPoint::Ack)) {
            ++out.unacked;
        } else {
            ring.ack(pos, 0);
        }
        fn(note);
    }
    return out;
}

} // namespace pitlane
