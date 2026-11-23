#pragma once

#include "pitlane/util/hint.hpp"
#include "pitlane/util/sendjoin.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace pitlane {

// Stable literal. The pointer is stored, not copied. Only pass string literals.
struct LogLit {
    const char* p{nullptr};
};

PITLANE_INLINE LogLit lit(const char* p) noexcept { return LogLit{p}; }

// One log line, one slot. Hot path writes tags and raw bytes. A cold thread formats text.
struct PackedLine {
    static constexpr std::size_t Cap = 512;
    uint8_t b[Cap]{};
    uint16_t used{1};
    uint8_t level{2};
    uint8_t ok{1};

    template <class T>
    void put(T v) noexcept {
        using D = std::remove_cv_t<std::remove_reference_t<T>>;
        if constexpr (std::is_same<D, LogLit>::value) {
            append(5, &v, sizeof(LogLit));
        } else if constexpr (std::is_same<D, bool>::value) {
            int32_t x = v ? 1 : 0;
            append(0, &x, sizeof x);
        } else if constexpr (std::is_integral<D>::value && std::is_signed<D>::value && sizeof(D) <= 4) {
            int32_t x = static_cast<int32_t>(v);
            append(0, &x, sizeof x);
        } else if constexpr (std::is_integral<D>::value && std::is_unsigned<D>::value && sizeof(D) <= 4) {
            uint32_t x = static_cast<uint32_t>(v);
            append(1, &x, sizeof x);
        } else if constexpr (std::is_integral<D>::value && std::is_signed<D>::value) {
            int64_t x = static_cast<int64_t>(v);
            append(2, &x, sizeof x);
        } else if constexpr (std::is_integral<D>::value && std::is_unsigned<D>::value) {
            uint64_t x = static_cast<uint64_t>(v);
            append(3, &x, sizeof x);
        } else if constexpr (std::is_floating_point<D>::value) {
            double x = static_cast<double>(v);
            append(4, &x, sizeof x);
        } else {
            static_assert(sizeof(D) == 0, "unsupported wire type");
        }
    }

    void put(LogLit s) noexcept { append(5, &s, sizeof s); }

    void put(const char* s) noexcept {
        if (!s) s = "";
        append(6, s, std::strlen(s) + 1);
    }

    void put(char* s) noexcept { put(static_cast<const char*>(s)); }

private:
    void append(uint8_t tag, const void* src, std::size_t n) noexcept {
        if (!ok || src == nullptr || used + 1 + n > Cap || b[0] == 255) {
            ok = 0;
            return;
        }
        b[used++] = tag;
        std::memcpy(b + used, src, n);
        used = static_cast<uint16_t>(used + n);
        ++b[0];
    }
};

// Async logger. log() never formats and never takes the heap. A full ring drops the line.
class WireLog {
public:
    static constexpr int kThread = 0;
    static constexpr int kCaller = 1;

    explicit WireLog(const char* path, int min_level = 3, int mode = kThread);
    ~WireLog();

    WireLog(const WireLog&) = delete;
    WireLog& operator=(const WireLog&) = delete;

    template <class... Ts>
    PITLANE_HOT bool log(int level, Ts... xs) noexcept {
        if (level > min_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        PackedLine rec;
        rec.level = static_cast<uint8_t>(level < 0 ? 0 : (level > 255 ? 255 : level));
        (rec.put(xs), ...);
        if (!rec.ok || !ring_ || !ring_->try_push(rec)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        published_.fetch_add(1, std::memory_order_release);
        return true;
    }

    void pump() noexcept;
    void flush() noexcept;
    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    bool pump_one() noexcept;
    static void* thread_main(void* self);

    int min_{3};
    int mode_{kThread};
    void* fp_{nullptr};
    SendJoinRing<PackedLine, 4096>* ring_{nullptr};
    void* thr_{nullptr};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> drained_{0};
    std::atomic<uint8_t> running_{0};
    uint8_t thr_on_{0};
};

} // namespace pitlane
