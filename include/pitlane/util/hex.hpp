#pragma once

#include <cstdint>

namespace pitlane {

// Hex and a byte-by-byte compare for buffers the caller owns.
// A failed call does not write a partial result.

inline int hex_encode(const uint8_t* src, int n, char* out, int cap) noexcept {
    if (n < 0 || cap <= 0 || !out || (n > 0 && !src)) return -1;
    if (n > (2147483647 - 1) / 2) return -1;
    if (cap < 2 * n + 1) return -1;
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < n; ++i) {
        out[2 * i] = kHex[src[i] >> 4];
        out[2 * i + 1] = kHex[src[i] & 0x0f];
    }
    out[2 * n] = 0;
    return 2 * n;
}

inline int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline int hex_decode(const char* src, int n, uint8_t* out, int cap) noexcept {
    if (n < 0 || (n > 0 && (!src || !out))) return -1;
    if ((n & 1) != 0) return -1;
    const int bytes = n / 2;
    if (bytes > cap) return -1;
    for (int i = 0; i < n; ++i) {
        if (hex_value(src[i]) < 0) return -1;
    }
    for (int i = 0; i < bytes; ++i) {
        const int hi = hex_value(src[2 * i]);
        const int lo = hex_value(src[2 * i + 1]);
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return bytes;
}

// First index that differs. -1 when the ranges match. -2 when the call is bad.
inline int bytes_diff(const uint8_t* a, const uint8_t* b, int n) noexcept {
    if (n < 0 || (n > 0 && (!a || !b))) return -2;
    for (int i = 0; i < n; ++i) {
        if (a[i] != b[i]) return i;
    }
    return -1;
}

}  // namespace pitlane
