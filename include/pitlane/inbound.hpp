#pragma once

#include <cstdint>

namespace pitlane {

// Commands waiting for their own arrival time. A release for one instrument leaves other instruments queued.
// Reports that come back from the counter are not delayed by this queue.

enum class FlightAct : uint8_t { Enter = 1, Pull = 2, Amend = 3 };

struct Flight {
    uint64_t tag{0};
    uint32_t name{0};
    int64_t due_us{0};
    FlightAct act{FlightAct::Enter};
};

template <int Cap>
class FlightQueue {
public:
    static_assert(Cap >= 2 && Cap <= 64, "flights");

    int size() const noexcept { return n_; }

    bool push(const Flight& flight) noexcept {
        if (n_ >= Cap || flight.tag == 0 || flight.name == 0) return false;
        if (flight.act != FlightAct::Enter && flight.act != FlightAct::Pull && flight.act != FlightAct::Amend) return false;
        int at = n_;
        while (at > 0 && slot_[at - 1].due_us > flight.due_us) {
            slot_[at] = slot_[at - 1];
            --at;
        }
        slot_[at] = flight;
        ++n_;
        return true;
    }

    int take(uint32_t name, int64_t now_us, Flight* out, int cap) noexcept {
        if (!out || cap <= 0 || name == 0) return -1;
        int wrote = 0;
        int i = 0;
        while (i < n_ && wrote < cap) {
            if (slot_[i].name == name && slot_[i].due_us <= now_us) {
                out[wrote++] = slot_[i];
                for (int j = i; j < n_ - 1; ++j) slot_[j] = slot_[j + 1];
                --n_;
            } else {
                ++i;
            }
        }
        return wrote;
    }

private:
    Flight slot_[Cap]{};
    int n_{0};
};

}  // namespace pitlane
