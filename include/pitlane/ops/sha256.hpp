#pragma once

// Self-contained SHA-256, HMAC-SHA256, and PBKDF2-HMAC-SHA256. The operations
// plane stores password verifiers, so the hub must not link a crypto library
// just for this. All state is caller-owned and the functions never allocate.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pitlane {

namespace sha_detail {

inline uint32_t rotr(uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32u - n)); }

constexpr uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t load_be(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void store_be(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void compress(uint32_t h[8], const uint8_t* block) noexcept {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = load_be(block + i * 4);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = k + s1 + ch + kK[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

} // namespace sha_detail

struct Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t total;
    uint32_t used;
};

inline void sha256_init(Sha256& s) noexcept {
    s.h[0] = 0x6a09e667u;
    s.h[1] = 0xbb67ae85u;
    s.h[2] = 0x3c6ef372u;
    s.h[3] = 0xa54ff53au;
    s.h[4] = 0x510e527fu;
    s.h[5] = 0x9b05688cu;
    s.h[6] = 0x1f83d9abu;
    s.h[7] = 0x5be0cd19u;
    s.total = 0;
    s.used = 0;
}

inline void sha256_update(Sha256& s, const void* data, std::size_t n) noexcept {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    s.total += n;
    while (n > 0) {
        const std::size_t take = (64 - s.used) < n ? (64 - s.used) : n;
        std::memcpy(s.buf + s.used, p, take);
        s.used += static_cast<uint32_t>(take);
        p += take;
        n -= take;
        if (s.used == 64) {
            sha_detail::compress(s.h, s.buf);
            s.used = 0;
        }
    }
}

inline void sha256_final(Sha256& s, uint8_t out[32]) noexcept {
    const uint64_t bits = s.total * 8;
    const uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    const uint8_t zero = 0;
    while (s.used != 56) sha256_update(s, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[7 - i] = static_cast<uint8_t>(bits >> (i * 8));
    sha256_update(s, len, 8);
    for (int i = 0; i < 8; ++i) sha_detail::store_be(out + i * 4, s.h[i]);
}

inline void sha256(const void* data, std::size_t n, uint8_t out[32]) noexcept {
    Sha256 s;
    sha256_init(s);
    sha256_update(s, data, n);
    sha256_final(s, out);
}

struct HmacSha256 {
    Sha256 inner;
    Sha256 outer;
};

inline void hmac_sha256_init(HmacSha256& h, const void* key, std::size_t n) noexcept {
    uint8_t k[64];
    std::memset(k, 0, sizeof k);
    if (n > 64) {
        sha256(key, n, k);
    } else if (n > 0) {
        std::memcpy(k, key, n);
    }
    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
    }
    sha256_init(h.inner);
    sha256_update(h.inner, ipad, 64);
    sha256_init(h.outer);
    sha256_update(h.outer, opad, 64);
    std::memset(k, 0, sizeof k);
    std::memset(ipad, 0, sizeof ipad);
    std::memset(opad, 0, sizeof opad);
}

inline void hmac_sha256_update(HmacSha256& h, const void* data, std::size_t n) noexcept {
    sha256_update(h.inner, data, n);
}

inline void hmac_sha256_final(HmacSha256& h, uint8_t out[32]) noexcept {
    uint8_t digest[32];
    sha256_final(h.inner, digest);
    sha256_update(h.outer, digest, 32);
    sha256_final(h.outer, out);
    std::memset(digest, 0, sizeof digest);
}

inline void hmac_sha256(const void* key, std::size_t key_n, const void* data, std::size_t n,
                        uint8_t out[32]) noexcept {
    HmacSha256 h;
    hmac_sha256_init(h, key, key_n);
    hmac_sha256_update(h, data, n);
    hmac_sha256_final(h, out);
}

// Derives one 32-byte key. One block is enough because SHA-256 is 32 bytes.
inline void pbkdf2_sha256(const char* pass, std::size_t pass_n, const uint8_t* salt, std::size_t salt_n,
                          uint32_t iters, uint8_t out[32]) noexcept {
    if (iters == 0) iters = 1;
    uint8_t u[32];
    uint8_t t[32];
    HmacSha256 h;
    hmac_sha256_init(h, pass, pass_n);
    hmac_sha256_update(h, salt, salt_n);
    const uint8_t one[4] = {0, 0, 0, 1};
    hmac_sha256_update(h, one, 4);
    hmac_sha256_final(h, u);
    std::memcpy(t, u, 32);
    for (uint32_t i = 1; i < iters; ++i) {
        hmac_sha256(pass, pass_n, u, 32, u);
        for (int j = 0; j < 32; ++j) t[j] = static_cast<uint8_t>(t[j] ^ u[j]);
    }
    std::memcpy(out, t, 32);
    std::memset(u, 0, sizeof u);
    std::memset(t, 0, sizeof t);
}

inline void hex_encode(const uint8_t* in, std::size_t n, char* out) noexcept {
    static const char kHex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < n; ++i) {
        out[i * 2] = kHex[in[i] >> 4];
        out[i * 2 + 1] = kHex[in[i] & 0x0f];
    }
    out[n * 2] = 0;
}

inline int hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool hex_decode(const char* in, std::size_t n, uint8_t* out, std::size_t out_cap) noexcept {
    if (n % 2 != 0 || n / 2 > out_cap) return false;
    for (std::size_t i = 0; i < n / 2; ++i) {
        const int hi = hex_nibble(in[i * 2]);
        const int lo = hex_nibble(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// Constant-time comparison for verifiers and tokens.
inline bool secure_equal(const uint8_t* a, const uint8_t* b, std::size_t n) noexcept {
    uint8_t diff = 0;
    for (std::size_t i = 0; i < n; ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

} // namespace pitlane
