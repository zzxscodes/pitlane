#pragma once

#include <cstdint>
#include <cstring>

namespace pitlane {

// A fixed text buffer. A write that does not fit leaves the previous text in place.

template <int Cap>
class TextBuf {
public:
    static_assert(Cap >= 8 && Cap <= 4096, "text");

    TextBuf() noexcept { buf_[0] = 0; }

    void clear() noexcept {
        n_ = 0;
        buf_[0] = 0;
    }

    int size() const noexcept { return n_; }
    const char* c_str() const noexcept { return buf_; }

    bool put_n(const char* src, int n) noexcept {
        if (n < 0 || (n > 0 && !src)) return false;
        if (n > Cap - 1 - n_) return false;
        if (n > 0) std::memcpy(buf_ + n_, src, static_cast<std::size_t>(n));
        n_ += n;
        buf_[n_] = 0;
        return true;
    }

    bool put(const char* src) noexcept {
        if (!src) return false;
        int n = 0;
        while (src[n] != 0) {
            if (n > Cap) return false;
            ++n;
        }
        return put_n(src, n);
    }

    bool put_char(char c) noexcept {
        if (c == 0) return false;
        return put_n(&c, 1);
    }

    bool put_i64(int64_t v) noexcept {
        if (v == INT64_MIN) return put("-9223372036854775808");
        const bool neg = v < 0;
        uint64_t x = neg ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);
        char rev[20];
        int n = 0;
        if (x == 0) {
            rev[n++] = '0';
        } else {
            while (x > 0) {
                rev[n++] = static_cast<char>('0' + static_cast<int>(x % 10));
                x /= 10;
            }
        }
        char out[24];
        int m = 0;
        if (neg) out[m++] = '-';
        for (int i = n - 1; i >= 0; --i) out[m++] = rev[i];
        return put_n(out, m);
    }

    bool put_hex(uint64_t v, int width) noexcept {
        if (width < 1 || width > 16) return false;
        char rev[16];
        int n = 0;
        uint64_t x = v;
        if (x == 0) {
            rev[n++] = '0';
        } else {
            while (x > 0) {
                rev[n++] = kHex[x & 0x0f];
                x >>= 4;
            }
        }
        if (n > width) return false;
        char out[16];
        const int pad = width - n;
        for (int i = 0; i < pad; ++i) out[i] = '0';
        for (int i = 0; i < n; ++i) out[pad + i] = rev[n - 1 - i];
        return put_n(out, width);
    }

private:
    static constexpr char kHex[] = "0123456789abcdef";

    char buf_[Cap]{};
    int n_{0};
};

}  // namespace pitlane
