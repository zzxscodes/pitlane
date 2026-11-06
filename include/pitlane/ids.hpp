#pragma once

#include <atomic>
#include <cstdint>

namespace pitlane {

// Restart-safe id. Bits, not a decimal concatenation:
// site 16 | kind 4 | day_mod 4 | ten-second slot 16 | serial 24.
// kind is a caller-defined class (plain, algo, query, basket, external echo).

inline uint64_t seal_id(uint16_t site, uint8_t kind, uint8_t day_mod, uint16_t ten_sec, uint32_t serial) noexcept {
    return (uint64_t(site) << 48) | (uint64_t(kind & 0xfu) << 44) | (uint64_t(day_mod & 0xfu) << 40) |
           (uint64_t(ten_sec) << 24) | uint64_t(serial & 0x00ffffffu);
}

inline uint16_t id_site(uint64_t id) noexcept { return uint16_t(id >> 48); }
inline uint8_t id_kind(uint64_t id) noexcept { return uint8_t((id >> 44) & 0xfu); }
inline uint8_t id_day_mod(uint64_t id) noexcept { return uint8_t((id >> 40) & 0xfu); }
inline uint16_t id_ten_sec(uint64_t id) noexcept { return uint16_t((id >> 24) & 0xffffu); }
inline uint32_t id_serial(uint64_t id) noexcept { return uint32_t(id & 0x00ffffffu); }

inline uint16_t ten_sec_slot(int sec_of_day) noexcept {
    if (sec_of_day < 0) return 0;
    if (sec_of_day >= 86400) sec_of_day %= 86400;
    return uint16_t(sec_of_day / 10);
}

class IdMint {
public:
    IdMint(uint16_t site, uint8_t kind) noexcept : site_(site), kind_(kind) {}

    uint64_t next(int day_index, int sec_of_day) noexcept {
        const uint32_t serial = serial_.fetch_add(1, std::memory_order_relaxed) & 0x00ffffffu;
        const uint8_t day_mod = uint8_t((day_index % 10 + 10) % 10);
        return seal_id(site_, kind_, day_mod, ten_sec_slot(sec_of_day), serial);
    }

private:
    uint16_t site_;
    uint8_t kind_;
    std::atomic<uint32_t> serial_{1};
};

}  // namespace pitlane
