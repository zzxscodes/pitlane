#include "pitlane/desk/cash.hpp"

#include <cstring>

namespace pitlane {
namespace {

void put_text(char* dst, int n, const char* src) noexcept {
    for (int i = 0; i < n; ++i) dst[i] = 0;
    if (!src) return;
    for (int i = 0; i < n - 1 && src[i]; ++i) dst[i] = src[i];
}

uint64_t fnv_bytes(uint64_t h, const char* s, int n) noexcept {
    if (!s) s = "";
    for (int i = 0; i < n && s[i]; ++i) {
        h ^= static_cast<unsigned char>(s[i]);
        h *= 1099511628211ull;
    }
    h ^= 0xffu;
    h *= 1099511628211ull;
    return h;
}

uint64_t fnv_start(uint8_t tier) noexcept {
    uint64_t h = 14695981039346656037ull;
    h ^= tier;
    h *= 1099511628211ull;
    return h;
}

}  // namespace

CashBoard::CashBoard(CashLine* lines, uint32_t cap) noexcept : lines_(lines), cap_(cap), soft_(cap) {}

bool CashBoard::tune(uint32_t soft) noexcept {
    if (soft < 1u || soft > cap_) return false;
    soft_ = soft;
    return true;
}

uint64_t CashBoard::key_of(uint8_t tier, const char* prod, const char* acct, const char* desk, const char* book) noexcept {
    uint64_t h = fnv_start(tier);
    h = fnv_bytes(h, prod, 16);
    h = fnv_bytes(h, acct, 24);
    h = fnv_bytes(h, desk, 16);
    h = fnv_bytes(h, book, 16);
    return h == 0 ? 1 : h;
}

void CashBoard::remember(uint64_t key, uint32_t index) noexcept {
    uint32_t i = static_cast<uint32_t>(key) & (kMap - 1u);
    for (uint32_t n = 0; n < kMap; ++n) {
        const uint32_t slot = (i + n) & (kMap - 1u);
        if (!map_used_[slot] || map_key_[slot] == key) {
            map_used_[slot] = 1;
            map_key_[slot] = key;
            map_at_[slot] = index;
            return;
        }
    }
}

void CashBoard::reindex() noexcept {
    std::memset(map_used_, 0, sizeof map_used_);
    if (!lines_) return;
    for (uint32_t i = 0; i < cap_; ++i) {
        if (lines_[i].live.load(std::memory_order_acquire) == 1 && lines_[i].key != 0) remember(lines_[i].key, i);
    }
}

CashLine* CashBoard::find(uint64_t key) const noexcept {
    if (!lines_ || key == 0) return nullptr;
    uint32_t i = static_cast<uint32_t>(key) & (kMap - 1u);
    for (uint32_t n = 0; n < kMap; ++n) {
        const uint32_t slot = (i + n) & (kMap - 1u);
        if (!map_used_[slot]) return nullptr;
        if (map_key_[slot] != key) continue;
        CashLine& row = lines_[map_at_[slot]];
        if (row.live.load(std::memory_order_acquire) == 1 && row.key == key) return &row;
        return nullptr;
    }
    return nullptr;
}

CashLine* CashBoard::upsert(uint8_t tier, const char* prod, const char* acct, const char* desk, const char* book) noexcept {
    const uint64_t key = key_of(tier, prod, acct, desk, book);
    if (CashLine* hit = find(key)) return hit;
    if (!lines_) return nullptr;
    for (uint32_t i = 0; i < soft_; ++i) {
        if (lines_[i].live.load(std::memory_order_acquire) == 1 && lines_[i].key == key) {
            remember(key, i);
            return &lines_[i];
        }
    }
    for (uint32_t i = 0; i < soft_; ++i) {
        uint8_t expect = 0;
        if (!lines_[i].live.compare_exchange_strong(expect, 2, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        put_text(lines_[i].prod, 16, prod);
        put_text(lines_[i].acct, 24, acct);
        put_text(lines_[i].desk, 16, desk);
        put_text(lines_[i].book, 16, book);
        lines_[i].tier = tier;
        lines_[i].free_amt = 0;
        lines_[i].held_amt = 0;
        lines_[i].key = key;
        lines_[i].live.store(1, std::memory_order_release);
        remember(key, i);
        return &lines_[i];
    }
    return nullptr;
}

bool CashBoard::reserve(CashLine& row, int64_t amt) noexcept {
    if (amt <= 0) return false;
    row.gate.enter();
    bool ok = false;
    if (row.live.load(std::memory_order_relaxed) == 1 && row.free_amt >= amt) {
        row.free_amt -= amt;
        row.held_amt += amt;
        ok = true;
    }
    row.gate.leave();
    return ok;
}

bool CashBoard::commit_held(CashLine& row, int64_t amt) noexcept {
    if (amt <= 0) return false;
    row.gate.enter();
    bool ok = false;
    if (row.held_amt >= amt) {
        row.held_amt -= amt;
        ok = true;
    }
    row.gate.leave();
    return ok;
}

bool CashBoard::release(CashLine& row, int64_t amt) noexcept {
    if (amt <= 0) return false;
    row.gate.enter();
    bool ok = false;
    if (row.held_amt >= amt) {
        row.held_amt -= amt;
        row.free_amt += amt;
        ok = true;
    }
    row.gate.leave();
    return ok;
}

void CashBoard::credit(CashLine& row, int64_t amt) noexcept {
    if (amt <= 0) return;
    row.gate.enter();
    row.free_amt += amt;
    row.gate.leave();
}

HoldBoard::HoldBoard(HoldLine* lines, uint32_t cap) noexcept : lines_(lines), cap_(cap), soft_(cap) {}

bool HoldBoard::tune(uint32_t soft) noexcept {
    if (soft < 1u || soft > cap_) return false;
    soft_ = soft;
    return true;
}

uint64_t HoldBoard::key_of(uint8_t tier, const char* prod, const char* acct, const char* name) noexcept {
    uint64_t h = fnv_start(tier);
    h = fnv_bytes(h, prod, 16);
    h = fnv_bytes(h, acct, 24);
    h = fnv_bytes(h, name, 16);
    return h == 0 ? 1 : h;
}

void HoldBoard::remember(uint64_t key, uint32_t index) noexcept {
    uint32_t i = static_cast<uint32_t>(key) & (kMap - 1u);
    for (uint32_t n = 0; n < kMap; ++n) {
        const uint32_t slot = (i + n) & (kMap - 1u);
        if (!map_used_[slot] || map_key_[slot] == key) {
            map_used_[slot] = 1;
            map_key_[slot] = key;
            map_at_[slot] = index;
            return;
        }
    }
}

void HoldBoard::reindex() noexcept {
    std::memset(map_used_, 0, sizeof map_used_);
    if (!lines_) return;
    for (uint32_t i = 0; i < cap_; ++i) {
        if (lines_[i].live.load(std::memory_order_acquire) == 1 && lines_[i].key != 0) remember(lines_[i].key, i);
    }
}

HoldLine* HoldBoard::find(uint64_t key) const noexcept {
    if (!lines_ || key == 0) return nullptr;
    uint32_t i = static_cast<uint32_t>(key) & (kMap - 1u);
    for (uint32_t n = 0; n < kMap; ++n) {
        const uint32_t slot = (i + n) & (kMap - 1u);
        if (!map_used_[slot]) return nullptr;
        if (map_key_[slot] != key) continue;
        HoldLine& row = lines_[map_at_[slot]];
        if (row.live.load(std::memory_order_acquire) == 1 && row.key == key) return &row;
        return nullptr;
    }
    return nullptr;
}

HoldLine* HoldBoard::upsert(uint8_t tier, const char* prod, const char* acct, const char* name, bool split_yday) noexcept {
    const uint64_t key = key_of(tier, prod, acct, name);
    if (HoldLine* hit = find(key)) return hit;
    if (!lines_) return nullptr;
    for (uint32_t i = 0; i < soft_; ++i) {
        if (lines_[i].live.load(std::memory_order_acquire) == 1 && lines_[i].key == key) {
            remember(key, i);
            return &lines_[i];
        }
    }
    for (uint32_t i = 0; i < soft_; ++i) {
        uint8_t expect = 0;
        if (!lines_[i].live.compare_exchange_strong(expect, 2, std::memory_order_acq_rel, std::memory_order_acquire))
            continue;
        put_text(lines_[i].prod, 16, prod);
        put_text(lines_[i].acct, 24, acct);
        put_text(lines_[i].name, 16, name);
        lines_[i].tier = tier;
        lines_[i].split_yday = split_yday ? 1 : 0;
        lines_[i].long_today = lines_[i].long_yday = 0;
        lines_[i].short_today = lines_[i].short_yday = 0;
        lines_[i].key = key;
        lines_[i].live.store(1, std::memory_order_release);
        remember(key, i);
        return &lines_[i];
    }
    return nullptr;
}

}  // namespace pitlane
