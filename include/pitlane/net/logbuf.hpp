#pragma once

#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Fixed-capacity term log. One producer appends framed payloads across a set
// of equally sized terms. Each reader keeps its own absolute cursor. A reader
// that falls behind the flow limit blocks the producer; nothing is overwritten
// while any reader still needs it.

namespace pitlane {

struct LogFrame {
    int32_t frame_len;
    uint8_t version;
    uint8_t flags;
    uint16_t type;
    int32_t term_offset;
    int32_t term_id;
    int32_t reserved;
    int64_t pad;
};

static_assert(sizeof(LogFrame) == 32, "LogFrame stays 32 bytes");

enum class LogFrameType : uint16_t { Data = 1, Pad = 2, Heartbeat = 3 };

template <uint32_t TermLen = 65536, uint32_t Terms = 3, uint32_t MaxReaders = 8>
class TermLog {
public:
    static_assert(TermLen >= 1024, "TermLen must be at least 1024");
    static_assert((TermLen & (TermLen - 1u)) == 0u, "TermLen must be a power of two");
    static_assert(Terms >= 3 && Terms <= 8, "Terms must be in 3..8");
    static_assert(MaxReaders >= 1 && MaxReaders <= 8, "MaxReaders must be in 1..8");

    TermLog() = default;
    TermLog(const TermLog&) = delete;
    TermLog& operator=(const TermLog&) = delete;

    // Startup-only. The new reader starts at the current tail.
    PITLANE_COLD int add_reader() noexcept {
        if (readers_ >= static_cast<int>(MaxReaders)) return -1;
        const int id = readers_++;
        reader_[id].store(tail_.load(std::memory_order_relaxed), std::memory_order_release);
        return id;
    }

    // Startup-only soft limit. Valid before the first publish.
    bool set_flow_limit(uint32_t bytes) noexcept {
        if (tail_.load(std::memory_order_relaxed) != 0) return false;
        if (static_cast<uint64_t>(bytes) < static_cast<uint64_t>(TermLen)) return false;
        if (static_cast<uint64_t>(bytes) >= static_cast<uint64_t>(TermLen) * Terms) return false;
        flow_limit_ = bytes;
        return true;
    }

    uint64_t tail() const noexcept { return tail_.load(std::memory_order_acquire); }

    uint64_t reader_pos(int reader) const noexcept {
        if (reader < 0 || reader >= readers_) return 0;
        return reader_[reader].load(std::memory_order_acquire);
    }

    uint64_t min_reader() const noexcept {
        if (readers_ <= 0) return tail();
        uint64_t low = UINT64_MAX;
        for (int i = 0; i < readers_; ++i) {
            const uint64_t pos = reader_[i].load(std::memory_order_acquire);
            if (pos < low) low = pos;
        }
        if (low == UINT64_MAX) return tail();
        return low;
    }

    PITLANE_HOT bool publish(const void* payload, uint32_t len, uint16_t type = 1) noexcept {
        if (len > 0 && !payload) return false;
        if (len > TermLen - 64u) return false;
        const uint16_t data_type = static_cast<uint16_t>(LogFrameType::Data);
        const uint16_t beat_type = static_cast<uint16_t>(LogFrameType::Heartbeat);
        if (type != data_type && type != beat_type) return false;

        uint64_t pos = tail_.load(std::memory_order_relaxed);
        const uint32_t frame_len = (32u + len + 31u) & ~31u;

        uint32_t offset = static_cast<uint32_t>(pos % TermLen);
        if (offset + frame_len > TermLen) {
            const uint32_t pad_len = TermLen - offset;
            const uint32_t term = static_cast<uint32_t>((pos / TermLen) % Terms);
            LogFrame pad{};
            pad.frame_len = static_cast<int32_t>(pad_len);
            pad.version = 1;
            pad.flags = 0;
            pad.type = static_cast<uint16_t>(LogFrameType::Pad);
            pad.term_offset = static_cast<int32_t>(offset);
            pad.term_id = static_cast<int32_t>(pos / TermLen);
            pad.reserved = 0;
            pad.pad = 0;
            std::memcpy(buffer_[term] + offset, &pad, sizeof(pad));
            std::memset(buffer_[term] + offset + sizeof(pad), 0, pad_len - sizeof(pad));
            pos += pad_len;
            // The tail moves once, after the data frame is complete. A reader
            // that reaches the pad will skip it and then see the new frame.
            offset = static_cast<uint32_t>(pos % TermLen);
        }
        if (offset + frame_len > TermLen) return false;

        if (readers_ > 0) {
            const uint64_t low = min_reader();
            if (pos + frame_len > low && pos + frame_len - low > flow_limit_) return false;
        }

        const uint32_t term = static_cast<uint32_t>((pos / TermLen) % Terms);
        LogFrame head{};
        head.frame_len = static_cast<int32_t>(frame_len);
        head.version = 1;
        head.flags = 0;
        head.type = type;
        head.term_offset = static_cast<int32_t>(offset);
        head.term_id = static_cast<int32_t>(pos / TermLen);
        head.reserved = static_cast<int32_t>(len);
        head.pad = 0;
        std::memcpy(buffer_[term] + offset, &head, sizeof(head));
        if (len > 0) std::memcpy(buffer_[term] + offset + sizeof(head), payload, len);
        const uint32_t pad = frame_len - 32u - len;
        if (pad > 0) std::memset(buffer_[term] + offset + 32u + len, 0, pad);
        tail_.store(pos + frame_len, std::memory_order_release);
        return true;
    }

    // 1: one frame copied. 0: nothing new. -1: gap, corruption, or small cap.
    PITLANE_HOT int poll(int reader, void* out, uint32_t cap, uint32_t& len, uint16_t& type) noexcept {
        if (reader < 0 || reader >= readers_) return 0;
        if (!out) return -1;
        uint64_t pos = reader_[reader].load(std::memory_order_acquire);
        for (;;) {
            const uint64_t t = tail_.load(std::memory_order_acquire);
            if (pos >= t) return 0;
            const uint32_t term = static_cast<uint32_t>((pos / TermLen) % Terms);
            const uint32_t offset = static_cast<uint32_t>(pos % TermLen);
            if (TermLen - offset < 32u) return -1;

            LogFrame head{};
            std::memcpy(&head, buffer_[term] + offset, sizeof(head));
            if (head.frame_len < 32) return -1;
            const uint32_t frame_len = static_cast<uint32_t>(head.frame_len);
            if (frame_len > TermLen - offset) return -1;
            if (head.term_offset != static_cast<int32_t>(offset)) return -1;
            if (head.term_id != static_cast<int32_t>(pos / TermLen)) return -1;
            if (head.reserved < 0 || static_cast<uint32_t>(head.reserved) > frame_len - 32u) return -1;

            const uint16_t data_type = static_cast<uint16_t>(LogFrameType::Data);
            const uint16_t beat_type = static_cast<uint16_t>(LogFrameType::Heartbeat);
            const uint16_t pad_type = static_cast<uint16_t>(LogFrameType::Pad);
            const uint16_t frame_type = head.type;
            if (frame_type != data_type && frame_type != beat_type && frame_type != pad_type) return -1;

            if (frame_type == pad_type) {
                pos += frame_len;
                reader_[reader].store(pos, std::memory_order_release);
                continue;
            }

            const uint32_t body = static_cast<uint32_t>(head.reserved);
            if (cap < body) return -1;
            if (body > 0) std::memcpy(out, buffer_[term] + offset + 32u, body);
            len = body;
            type = frame_type;
            pos += frame_len;
            reader_[reader].store(pos, std::memory_order_release);
            return 1;
        }
    }

    void resync(int reader) noexcept {
        if (reader < 0 || reader >= readers_) return;
        reader_[reader].store(tail_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    alignas(64) uint8_t buffer_[Terms][TermLen]{};
    alignas(64) std::atomic<uint64_t> tail_{0};
    alignas(64) std::atomic<uint64_t> reader_[MaxReaders]{};
    int readers_{0};
    uint32_t flow_limit_{TermLen * (Terms - 1)};
};

}  // namespace pitlane
