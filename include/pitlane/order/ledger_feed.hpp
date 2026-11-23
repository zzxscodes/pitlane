#pragma once

// Counter-published account and position ledger. A gateway thread (CTP
// callbacks, for example) replaces whole snapshots after a query reply, while
// HTTP threads page them. The critical sections are cold and short, so a spin
// lock is enough and the header stays free of <mutex>.
//
// Money fields follow LedgerAsset: whole yuan. Prices are 1e-4.

#include "pitlane/order/ledger_view.hpp"

#include <atomic>
#include <cstdint>

namespace pitlane {

class LedgerFeed {
public:
    static constexpr int kCap = 64;

    void replace(const LedgerAsset* assets, int n_assets, const LedgerPosition* positions, int n_positions,
                 int64_t stamp_us) noexcept {
        lock();
        view_.clear();
        for (int i = 0; i < n_assets && i < kCap; ++i) (void)view_.upsert(assets[i]);
        for (int i = 0; i < n_positions && i < kCap; ++i) (void)view_.upsert(positions[i]);
        stamp_us_.store(stamp_us, std::memory_order_relaxed);
        version_.fetch_add(1, std::memory_order_release);
        unlock();
    }

    // Account and position queries finish at different times, so each kind can
    // replace only its own half of the snapshot.
    void replace_assets(const LedgerAsset* rows, int n, int64_t stamp_us) noexcept {
        lock();
        view_.clear_assets();
        for (int i = 0; i < n && i < kCap; ++i) (void)view_.upsert(rows[i]);
        stamp_us_.store(stamp_us, std::memory_order_relaxed);
        version_.fetch_add(1, std::memory_order_release);
        unlock();
    }

    void replace_positions(const LedgerPosition* rows, int n, int64_t stamp_us) noexcept {
        lock();
        view_.clear_positions();
        for (int i = 0; i < n && i < kCap; ++i) (void)view_.upsert(rows[i]);
        stamp_us_.store(stamp_us, std::memory_order_relaxed);
        version_.fetch_add(1, std::memory_order_release);
        unlock();
    }

    int query_assets(const LedgerFilter& f, int cursor, LedgerAsset* out, int cap, int* next,
                     bool* is_last) const noexcept {
        lock();
        const int n = view_.query_assets(f, cursor, out, cap, next, is_last);
        unlock();
        return n;
    }

    int query_positions(const LedgerFilter& f, int cursor, LedgerPosition* out, int cap, int* next,
                        bool* is_last) const noexcept {
        lock();
        const int n = view_.query_positions(f, cursor, out, cap, next, is_last);
        unlock();
        return n;
    }

    int assets() const noexcept {
        lock();
        const int n = view_.assets();
        unlock();
        return n;
    }

    int positions() const noexcept {
        lock();
        const int n = view_.positions();
        unlock();
        return n;
    }

    int64_t stamp_us() const noexcept { return stamp_us_.load(std::memory_order_relaxed); }

    uint32_t version() const noexcept { return version_.load(std::memory_order_acquire); }

    void clear() noexcept {
        lock();
        view_.clear();
        version_.fetch_add(1, std::memory_order_release);
        unlock();
    }

private:
    void lock() const noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
        }
    }

    void unlock() const noexcept { flag_.clear(std::memory_order_release); }

    mutable std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    LedgerView<kCap> view_{};
    std::atomic<int64_t> stamp_us_{0};
    std::atomic<uint32_t> version_{0};
};

}  // namespace pitlane
