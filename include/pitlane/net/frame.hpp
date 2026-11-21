#pragma once

#include "pitlane/util/hint.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Bounded network frames. Prices and quantities are integers, times are
// microseconds, and a side value of 1 buys, 2 sells. The body is copied
// verbatim, so every body must be trivially copyable and at most kNetMaxBody.

namespace pitlane {

enum class NetKind : uint8_t {
    Intent = 1,
    Report = 2,
    Quote = 3,
    Signal = 4,
    Control = 5,
    Snapshot = 6,
    Heartbeat = 7
};

inline constexpr uint16_t kNetVersion = 1;
inline constexpr uint32_t kNetMagic = 0x504C4E31u;
inline constexpr std::size_t kNetMaxBody = 512;

struct NetHeader {
    uint32_t len;
    uint32_t crc;
    uint64_t seq;
    int64_t stamp_us;
    uint8_t kind;
    uint8_t version;
    uint16_t flags;
    uint32_t magic;
};

static_assert(sizeof(NetHeader) == 32, "NetHeader stays 32 bytes");

template <class T>
struct NetFrame {
    NetHeader head;
    T body;
};

// Bitwise CRC32C (Castagnoli). No table and no allocation.
inline uint32_t net_crc32c(const void* data, std::size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return ~crc;
}

template <class T>
bool net_pack(NetKind kind, uint64_t seq, int64_t stamp_us, const T& body, void* out, std::size_t cap,
              std::size_t* written) noexcept {
    static_assert(std::is_trivially_copyable<T>::value, "body must be trivially copyable");
    static_assert(sizeof(T) <= kNetMaxBody, "body exceeds kNetMaxBody");

    const uint8_t k = static_cast<uint8_t>(kind);
    if (!out || !written || k < 1u || k > 7u) return false;
    if (cap < sizeof(NetHeader) + sizeof(T)) return false;

    NetHeader head{};
    head.len = static_cast<uint32_t>(sizeof(NetHeader) + sizeof(T));
    head.crc = net_crc32c(&body, sizeof(T));
    head.seq = seq;
    head.stamp_us = stamp_us;
    head.kind = k;
    head.version = static_cast<uint8_t>(kNetVersion);
    head.flags = 0;
    head.magic = kNetMagic;

    std::memcpy(out, &head, sizeof(head));
    std::memcpy(static_cast<uint8_t*>(out) + sizeof(NetHeader), &body, sizeof(T));
    *written = head.len;
    return true;
}

// Copies only on success. A rejected frame leaves body untouched.
template <class T>
bool net_unpack(const void* in, std::size_t len, NetHeader& head, T& body, std::size_t* consumed) noexcept {
    static_assert(std::is_trivially_copyable<T>::value, "body must be trivially copyable");
    static_assert(sizeof(T) <= kNetMaxBody, "body exceeds kNetMaxBody");

    if (!in || !consumed || len < sizeof(NetHeader)) return false;

    NetHeader h{};
    std::memcpy(&h, in, sizeof(h));
    if (h.magic != kNetMagic) return false;
    if (h.version != static_cast<uint8_t>(kNetVersion)) return false;
    if (h.kind < 1u || h.kind > 7u) return false;
    if (h.len != static_cast<uint32_t>(sizeof(NetHeader) + sizeof(T))) return false;
    if (len < h.len) return false;

    const auto* src = static_cast<const uint8_t*>(in) + sizeof(NetHeader);
    if (net_crc32c(src, sizeof(T)) != h.crc) return false;

    T tmp{};
    std::memcpy(&tmp, src, sizeof(T));
    body = tmp;
    head = h;
    *consumed = h.len;
    return true;
}

}  // namespace pitlane
