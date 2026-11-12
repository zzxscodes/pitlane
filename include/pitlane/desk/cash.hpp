#pragma once

#include <atomic>
#include <climits>
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace pitlane {

// Short critical section around a shared cash or inventory row.
struct RowGate {
    std::atomic<uint32_t> held{0};

    void enter() noexcept {
        for (;;) {
            uint32_t expect = 0;
            if (held.compare_exchange_weak(expect, 1, std::memory_order_acquire, std::memory_order_relaxed)) return;
#if defined(__x86_64__) || defined(__i386__)
            _mm_pause();
#endif
        }
    }

    void leave() noexcept { held.store(0, std::memory_order_release); }
};

struct CashLine {
    RowGate gate;
    std::atomic<uint8_t> live{0};  // 0 empty, 2 writing, 1 published
    uint64_t key{0};
    int64_t free_amt{0};
    int64_t held_amt{0};
    char prod[16]{};
    char acct[24]{};
    char desk[16]{};
    char book[16]{};
    uint8_t tier{0};
};

struct HoldLine {
    RowGate gate;
    std::atomic<uint8_t> live{0};
    uint64_t key{0};
    int64_t long_today{0};
    int64_t long_yday{0};
    int64_t short_today{0};
    int64_t short_yday{0};
    char prod[16]{};
    char acct[24]{};
    char name[16]{};
    uint8_t tier{0};
    uint8_t split_yday{0};
};

class CashBoard {
public:
    CashBoard(CashLine* lines, uint32_t cap) noexcept;

    static uint64_t key_of(uint8_t tier, const char* prod, const char* acct, const char* desk, const char* book) noexcept;

    // Startup soft row limit in [1, physical cap]. Does not shrink the array.
    bool tune(uint32_t soft) noexcept;
    uint32_t capacity() const noexcept { return soft_; }
    uint32_t hard_cap() const noexcept { return cap_; }

    void reindex() noexcept;
    CashLine* find(uint64_t key) const noexcept;
    CashLine* upsert(uint8_t tier, const char* prod, const char* acct, const char* desk, const char* book) noexcept;

    static bool reserve(CashLine& row, int64_t amt) noexcept;
    static bool commit_held(CashLine& row, int64_t amt) noexcept;
    static bool release(CashLine& row, int64_t amt) noexcept;
    static void credit(CashLine& row, int64_t amt) noexcept;

private:
    void remember(uint64_t key, uint32_t index) noexcept;

    CashLine* lines_;
    uint32_t cap_;
    uint32_t soft_;
    static constexpr uint32_t kMap = 512;
    uint64_t map_key_[kMap]{};
    uint32_t map_at_[kMap]{};
    uint8_t map_used_[kMap]{};
};

// Account-level buying power check (notional + fee against free_amt). Not a virtual sub-book.
inline bool cash_enough(const CashLine& row, int64_t notional, int64_t fee) noexcept {
    if (notional < 0 || fee < 0) return false;
    if (notional > INT64_MAX - fee) return false;
    return row.free_amt >= notional + fee;
}

class HoldBoard {
public:
    HoldBoard(HoldLine* lines, uint32_t cap) noexcept;

    static uint64_t key_of(uint8_t tier, const char* prod, const char* acct, const char* name) noexcept;

    bool tune(uint32_t soft) noexcept;
    uint32_t capacity() const noexcept { return soft_; }
    uint32_t hard_cap() const noexcept { return cap_; }

    void reindex() noexcept;
    HoldLine* find(uint64_t key) const noexcept;
    HoldLine* upsert(uint8_t tier, const char* prod, const char* acct, const char* name, bool split_yday) noexcept;

private:
    void remember(uint64_t key, uint32_t index) noexcept;

    HoldLine* lines_;
    uint32_t cap_;
    uint32_t soft_;
    static constexpr uint32_t kMap = 512;
    uint64_t map_key_[kMap]{};
    uint32_t map_at_[kMap]{};
    uint8_t map_used_[kMap]{};
};

}  // namespace pitlane
