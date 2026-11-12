#pragma once

#include "pitlane/desk/ring.hpp"
#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace pitlane {

// Local topic bus (topic bus RabbitMQ / AMQP role on one machine).
// Routing key → topic id; publish/consume on a FanRing. No sockets. Hot path no alloc.

inline constexpr uint32_t kBusCap = 1024;
inline constexpr uint16_t kBusPayload = 96;

struct BusNote {
    uint32_t topic{0};
    uint16_t kind{0}; // caller-defined: 1 req, 2 rsp, 3 md, …
    uint16_t len{0};
    int64_t stamp_us{0};
    uint8_t data[kBusPayload]{};
};

static_assert(std::is_trivially_copyable<BusNote>::value, "BusNote");

inline uint32_t topic_id(const char* key) noexcept {
    if (!key || !key[0]) return 0;
    // FNV-1a 32-bit over the routing key text.
    uint32_t h = 2166136261u;
    for (const char* p = key; *p; ++p) {
        h ^= static_cast<uint8_t>(*p);
        h *= 16777619u;
    }
    return h == 0 ? 1u : h;
}

class TopicBus {
public:
    bool tune(uint32_t soft) noexcept { return notes_.tune(soft); }
    uint32_t capacity() const noexcept { return notes_.capacity(); }

    PITLANE_HOT bool publish(uint32_t topic, uint16_t kind, const void* payload, uint16_t len, int64_t stamp_us,
                             unsigned mask = 0) noexcept {
        if (topic == 0 || len > kBusPayload) return false;
        if (len > 0 && !payload) return false;
        BusNote note{};
        note.topic = topic;
        note.kind = kind;
        note.len = len;
        note.stamp_us = stamp_us;
        if (len) std::memcpy(note.data, payload, len);
        return notes_.publish(note, mask);
    }

    PITLANE_HOT bool publish_key(const char* key, uint16_t kind, const void* payload, uint16_t len, int64_t stamp_us,
                                 unsigned mask = 0) noexcept {
        return publish(topic_id(key), kind, payload, len, stamp_us, mask);
    }

    PITLANE_HOT bool at(uint64_t pos, unsigned reader, BusNote& out, bool* fresh) const noexcept {
        return notes_.at(pos, reader, out, fresh);
    }

    PITLANE_HOT bool ack(uint64_t pos, unsigned reader) noexcept { return notes_.ack(pos, reader); }

    // Drain notes matching topic (0 = any). Returns count copied; stops at cap or empty.
    PITLANE_HOT int drain(unsigned reader, uint64_t& cursor, uint32_t topic, BusNote* out, int cap) noexcept {
        if (!out || cap <= 0) return 0;
        int n = 0;
        while (n < cap) {
            BusNote note{};
            bool fresh = false;
            if (!notes_.at(cursor, reader, note, &fresh) || !fresh) break;
            if (topic != 0 && note.topic != topic) {
                notes_.ack(cursor, reader);
                ++cursor;
                continue;
            }
            out[n++] = note;
            notes_.ack(cursor, reader);
            ++cursor;
        }
        return n;
    }

private:
    FanRing<BusNote, kBusCap> notes_{};
};

} // namespace pitlane
