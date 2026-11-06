#pragma once

#include <cstdint>
#include <cstring>

namespace pitlane {

// Cuts a caller-owned byte buffer into frames. The library does not read a socket.
// A declared length past the limit leaves the buffer unchanged.
// A bad checksum drops that frame and keeps the bytes that follow it.

enum class FrameDigest : uint8_t { Sum8 = 1, Crc16 = 2, Crc32 = 3 };

enum class AsmCode : int8_t { Need = 0, Frame = 1, BadLen = -1, BadDig = -2, BadArg = -3, Busy = -4 };

struct FrameSpec {
    int hdr{4};
    int len_off{0};
    int max_body{0};
    FrameDigest kind{FrameDigest::Sum8};
};

inline int dig_bytes(FrameDigest kind) noexcept {
    if (kind == FrameDigest::Sum8) return 1;
    if (kind == FrameDigest::Crc16) return 2;
    if (kind == FrameDigest::Crc32) return 4;
    return 0;
}

struct Crc16Table {
    uint16_t v[256];
    constexpr Crc16Table() : v{} {
        for (int i = 0; i < 256; ++i) {
            unsigned c = static_cast<unsigned>(i) << 8;
            for (int k = 0; k < 8; ++k) {
                if ((c & 0x8000u) != 0) c = ((c << 1) ^ 0x1021u) & 0xffffu;
                else c = (c << 1) & 0xffffu;
            }
            v[i] = static_cast<uint16_t>(c);
        }
    }
};

struct Crc32Table {
    uint32_t v[256];
    constexpr Crc32Table() : v{} {
        for (int i = 0; i < 256; ++i) {
            uint32_t c = static_cast<uint32_t>(i);
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            v[i] = c;
        }
    }
};

inline constexpr Crc16Table kCrc16{};
inline constexpr Crc32Table kCrc32{};

inline uint32_t digest_of(FrameDigest kind, const uint8_t* p, int n) noexcept {
    if (n < 0 || (n > 0 && !p) || dig_bytes(kind) == 0) return 0;
    if (kind == FrameDigest::Sum8) {
        uint32_t s = 0;
        for (int i = 0; i < n; ++i) s += p[i];
        return s & 0xffu;
    }
    if (kind == FrameDigest::Crc16) {
        uint16_t c = 0xffffu;
        for (int i = 0; i < n; ++i) {
            const unsigned idx = ((static_cast<unsigned>(c) >> 8) ^ p[i]) & 0xffu;
            c = static_cast<uint16_t>(kCrc16.v[idx] ^ static_cast<uint16_t>(c << 8));
        }
        return c;
    }
    uint32_t c = 0xffffffffu;
    for (int i = 0; i < n; ++i) c = kCrc32.v[(c ^ p[i]) & 0xffu] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

inline bool digest_write(FrameDigest kind, const uint8_t* p, int n, uint8_t* dest) noexcept {
    const int dn = dig_bytes(kind);
    if (!dest || dn == 0 || n < 0 || (n > 0 && !p)) return false;
    const uint32_t v = digest_of(kind, p, n);
    for (int i = 0; i < dn; ++i) dest[i] = static_cast<uint8_t>(v >> (8 * i));
    return true;
}

inline uint32_t load_u32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint32_t load_dig(const uint8_t* p, int n) noexcept {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

template <int Cap>
class StreamAsm {
public:
    static_assert(Cap >= 16 && Cap <= 65536, "stream");

    int stored() const noexcept { return n_; }

    bool append(const uint8_t* p, int n) noexcept {
        if (view_ > 0 || n < 0 || (n > 0 && !p)) return false;
        if (n > Cap - n_) return false;
        if (n > 0) std::memcpy(buf_ + n_, p, static_cast<std::size_t>(n));
        n_ += n;
        return true;
    }

    AsmCode pull(const FrameSpec& spec, const uint8_t** frame, int* len) noexcept {
        if (frame) *frame = nullptr;
        if (len) *len = 0;
        if (view_ > 0) return AsmCode::Busy;
        if (!frame || !len) return AsmCode::BadArg;
        const int dn = dig_bytes(spec.kind);
        if (spec.hdr < 4 || spec.hdr > 32 || spec.len_off < 0 || spec.len_off > spec.hdr - 4 || spec.max_body < 0 || dn == 0) {
            return AsmCode::BadArg;
        }
        if (spec.hdr > Cap || dn > Cap - spec.hdr) return AsmCode::BadArg;
        if (n_ < spec.hdr) return AsmCode::Need;
        const uint32_t body = load_u32(buf_ + spec.len_off);
        if (body > static_cast<uint32_t>(spec.max_body)) return AsmCode::BadLen;
        if (static_cast<uint32_t>(dn) > static_cast<uint32_t>(Cap - spec.hdr) ||
            body > static_cast<uint32_t>(Cap - spec.hdr - dn)) {
            return AsmCode::BadLen;
        }
        const int total = spec.hdr + static_cast<int>(body) + dn;
        if (n_ < total) return AsmCode::Need;
        const int covered = spec.hdr + static_cast<int>(body);
        const uint32_t got = digest_of(spec.kind, buf_, covered);
        const uint32_t stored = load_dig(buf_ + covered, dn);
        if (got != stored) {
            const int rest = n_ - total;
            if (rest > 0) std::memmove(buf_, buf_ + total, static_cast<std::size_t>(rest));
            n_ = rest;
            return AsmCode::BadDig;
        }
        *frame = buf_;
        *len = total;
        view_ = total;
        return AsmCode::Frame;
    }

    void commit() noexcept {
        if (view_ <= 0) return;
        const int rest = n_ - view_;
        if (rest > 0) std::memmove(buf_, buf_ + view_, static_cast<std::size_t>(rest));
        n_ = rest;
        view_ = 0;
    }

    void reset() noexcept {
        n_ = 0;
        view_ = 0;
    }

private:
    uint8_t buf_[Cap]{};
    int n_{0};
    int view_{0};
};

}  // namespace pitlane
