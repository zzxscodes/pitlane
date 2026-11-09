#pragma once

#include <cstdint>
#include <cstring>

namespace pitlane {

// A length-prefixed frame. The length is written when the frame is closed.
struct ByteFrame {
    bool begin(uint8_t* storage, int cap, uint16_t kind) noexcept {
        buf_ = nullptr;
        cap_ = 0;
        at_ = 0;
        open_ = false;
        if (!storage || cap < 4) return false;
        buf_ = storage;
        cap_ = cap;
        buf_[0] = static_cast<uint8_t>(kind);
        buf_[1] = static_cast<uint8_t>(kind >> 8);
        buf_[2] = 0;
        buf_[3] = 0;
        at_ = 4;
        open_ = true;
        return true;
    }

    bool put(const void* data, int n) noexcept {
        if (!open_ || n < 0 || (n > 0 && !data)) return false;
        if (n > cap_ - at_) return false;
        if (n > 0) std::memcpy(buf_ + at_, data, static_cast<std::size_t>(n));
        at_ += n;
        return true;
    }

    int end() noexcept {
        if (!open_) return -1;
        const int payload = at_ - 4;
        if (payload > 65535) return -1;
        buf_[2] = static_cast<uint8_t>(payload);
        buf_[3] = static_cast<uint8_t>(static_cast<unsigned>(payload) >> 8);
        open_ = false;
        return at_;
    }

private:
    uint8_t* buf_{nullptr};
    int cap_{0};
    int at_{0};
    bool open_{false};
};

struct FrameView {
    uint16_t kind{0};
    uint16_t len{0};
    const uint8_t* body{nullptr};
};

inline bool frame_read(const uint8_t* buf, int n, FrameView& out) noexcept {
    out = FrameView{};
    if (!buf || n < 4) return false;
    const uint16_t kind = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
    const uint16_t len = static_cast<uint16_t>(buf[2] | (buf[3] << 8));
    if (4 + static_cast<int>(len) > n) return false;
    out.kind = kind;
    out.len = len;
    out.body = buf + 4;
    return true;
}

// 0 means the sequence continues. A positive result is the number of skipped ids.
// -1 means the id is older than the one expected next.
struct SeqWatch {
    uint64_t next{0};
    uint8_t started{0};
};

inline int seq_feed(SeqWatch& watch, uint64_t seq, uint64_t* missed) noexcept {
    if (missed) *missed = 0;
    if (!watch.started) {
        watch.started = 1;
        watch.next = seq + 1;
        return 0;
    }
    if (seq < watch.next) return -1;
    if (seq > watch.next) {
        const uint64_t gap = seq - watch.next;
        if (missed) *missed = gap;
        watch.next = seq + 1;
        return gap > 0x7fffffffull ? 0x7fffffff : static_cast<int>(gap);
    }
    watch.next = seq + 1;
    return 0;
}

// Holds a short run of messages that arrived ahead of the next expected id.
template <int Cap, int Bytes>
class SeqHold {
public:
    static_assert(Cap >= 2 && Cap <= 32, "window");
    static_assert(Bytes >= 1 && Bytes <= 256, "bytes");

    bool start(uint64_t next) noexcept {
        if (armed_) return false;
        expect_ = next;
        armed_ = true;
        return true;
    }

    bool offer(uint64_t seq, const void* data, int n) noexcept {
        if (!armed_ || n < 0 || n > Bytes || (n > 0 && !data)) return false;
        if (seq < expect_ || seq - expect_ >= static_cast<uint64_t>(Cap)) return false;
        const int at = static_cast<int>(seq % static_cast<uint64_t>(Cap));
        if (used_[at] && slot_seq_[at] == seq) return false;
        if (used_[at] && slot_seq_[at] != seq) return false;
        if (n > 0) std::memcpy(slot_[at], data, static_cast<std::size_t>(n));
        len_[at] = static_cast<uint16_t>(n);
        slot_seq_[at] = seq;
        used_[at] = 1;
        return true;
    }

    int take(uint8_t* out, int cap, uint64_t* seq) noexcept {
        if (!armed_) return 0;
        const int at = static_cast<int>(expect_ % static_cast<uint64_t>(Cap));
        if (!used_[at] || slot_seq_[at] != expect_) return 0;
        if (len_[at] > cap || (len_[at] > 0 && !out)) return -1;
        if (len_[at] > 0) std::memcpy(out, slot_[at], len_[at]);
        if (seq) *seq = expect_;
        used_[at] = 0;
        const int n = len_[at];
        ++expect_;
        return n;
    }

    uint64_t expect() const noexcept { return expect_; }

private:
    uint8_t slot_[Cap][Bytes]{};
    uint64_t slot_seq_[Cap]{};
    uint16_t len_[Cap]{};
    uint8_t used_[Cap]{};
    uint64_t expect_{0};
    bool armed_{false};
};

}  // namespace pitlane
