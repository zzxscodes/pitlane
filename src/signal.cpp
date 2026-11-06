#include "pitlane/signal.hpp"

#include <cstring>

namespace pitlane {

bool PulseBoard::watch(uint32_t name, const char* topic, int64_t from_us, bool dense_only) noexcept {
    if (nsub_ >= kSubs || name == 0) return false;
    Sub& s = sub_[nsub_++];
    s = Sub{};
    s.name = name;
    s.from_us = from_us;
    s.dense_only = dense_only ? 1 : 0;
    s.live = 1;
    if (topic) {
        std::strncpy(s.topic, topic, 23);
        s.topic[23] = 0;
    }
    return true;
}

void PulseBoard::clear() noexcept {
    nsub_ = 0;
    head_ = 0;
    count_ = 0;
}

bool PulseBoard::offer(const Pulse& pulse) noexcept {
    bool hit = false;
    for (int i = 0; i < nsub_; ++i) {
        const Sub& s = sub_[i];
        if (!s.live || s.name != pulse.name) continue;
        if (s.topic[0] && std::strcmp(s.topic, pulse.topic) != 0) continue;
        if (s.dense_only && !pulse.dense) continue;
        if (pulse.stamp_us < s.from_us) continue;
        hit = true;
        break;
    }
    if (!hit || count_ >= kQueue) return false;
    const int at = (head_ + count_) % kQueue;
    q_[at] = pulse;
    ++count_;
    return true;
}

int PulseBoard::drain(Pulse* out, int cap) noexcept {
    int n = 0;
    while (n < cap && count_ > 0) {
        if (out) out[n] = q_[head_];
        head_ = (head_ + 1) % kQueue;
        --count_;
        ++n;
    }
    return n;
}

}  // namespace pitlane
