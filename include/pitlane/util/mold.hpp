#pragma once

#include <cstdint>
#include <cstring>

namespace pitlane {

// A prepared frame. The hot path overwrites a few fixed offsets and does not rebuild the message.
// The caller still runs risk checks and the order state before sending.

struct IdSlip {
    uint64_t next{1};
    uint64_t last{0};
    uint8_t open{0};

    uint64_t issue() noexcept {
        if (next == UINT64_MAX) return 0;
        last = next++;
        open = 1;
        return last;
    }

    void keep() noexcept { open = 0; }

    bool undo() noexcept {
        if (!open || next != last + 1) return false;
        next = last;
        open = 0;
        last = 0;
        return true;
    }
};

struct WireMold {
    uint8_t* buf{nullptr};
    int cap{0};
    int len{0};

    bool seal(uint8_t* storage, int storage_cap, const uint8_t* prefix, int n) noexcept {
        buf = nullptr;
        cap = 0;
        len = 0;
        if (!storage || storage_cap <= 0 || n < 0 || n > storage_cap || (n > 0 && !prefix)) return false;
        if (n > 0) std::memcpy(storage, prefix, static_cast<std::size_t>(n));
        buf = storage;
        cap = storage_cap;
        len = n;
        return true;
    }

    bool put_u64(int off, uint64_t v) noexcept {
        if (!buf || off < 0 || off > len - 8) return false;
        for (int i = 0; i < 8; ++i) buf[off + i] = static_cast<uint8_t>(v >> (8 * i));
        return true;
    }

    bool put_i64(int off, int64_t v) noexcept { return put_u64(off, static_cast<uint64_t>(v)); }
};

struct BlockHead {
    uint16_t block{0};
    uint16_t tmpl{0};
    uint16_t schema{0};
    uint16_t ver{0};
};

inline uint16_t load_u16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

// know is the fixed width this build understands. A longer block is accepted and the extra bytes are skipped.
inline bool read_block(const uint8_t* p, int n, uint16_t know, BlockHead& head, const uint8_t** body) noexcept {
    if (!p || n < 8 || know == 0) return false;
    head.block = load_u16(p);
    head.tmpl = load_u16(p + 2);
    head.schema = load_u16(p + 4);
    head.ver = load_u16(p + 6);
    if (head.block < know) return false;
    if (n < 8 + static_cast<int>(head.block)) return false;
    if (body) *body = p + 8;
    return true;
}

inline const uint8_t* block_end(const uint8_t* p, int n) noexcept {
    if (!p || n < 8) return nullptr;
    const int block = load_u16(p);
    if (n < 8 + block) return nullptr;
    return p + 8 + block;
}

}  // namespace pitlane
