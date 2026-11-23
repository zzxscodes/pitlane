#pragma once

// 行情实时视图。与具体柜台无关：任何适配器都往同一份定长快照里填数，
// 行情线程写、控制台线程读，两者之间没有锁：发布方把一条固定大小的快照写进
// seqlock，读取方复制一份并确认序号未动。记录里没有指针，可以直接编码成 JSON，
// 也可以安全地跨线程拷贝。

#include <atomic>
#include <cstdint>

namespace pitlane {

// 与柜台无关的行情视图常量。适配器把自家状态映射到这些值。
inline constexpr int kMarketBooks = 4;
inline constexpr int kMarketPrints = 12;
inline constexpr int kMarketDepth = 5;

enum : uint8_t {
    kMarketNoQuote = 0,
    kMarketTrading = 1,
    kMarketLimitUp = 2,
    kMarketLimitDown = 3,
    kMarketAuction = 4
};

enum : uint8_t {
    kMarketBookOk = 0,
    kMarketBookCrossed = 1,
    kMarketBookStale = 2,
    kMarketBookRebuilt = 3,
    kMarketBookNoQuote = 4
};

struct MarketLevel {
    int64_t px{0};
    int64_t qty{0};
};

struct MarketBook {
    uint32_t name{0};
    uint8_t status{kMarketNoQuote};
    uint8_t levels{0};
    uint8_t event{kMarketBookOk};
    uint8_t pad{0};
    int64_t exch_us{0};
    int64_t last_px{0};
    int64_t upper_limit{0};
    int64_t lower_limit{0};
    MarketLevel bid[kMarketDepth]{};
    MarketLevel ask[kMarketDepth]{};
    uint64_t snaps{0};
    uint64_t updates{0};
    uint64_t rebuilds{0};
    uint64_t stale{0};
    uint64_t crossed{0};
    int32_t imbalance_ppm{0};
    int32_t pad2{0};
};

struct MarketPrint {
    uint32_t name{0};
    uint8_t aggressor{0};
    uint8_t pad[3]{};
    int64_t exch_us{0};
    int64_t px{0};
    int64_t qty{0};
    int64_t volume{0};
};

struct MarketSnap {
    uint64_t seq{0};
    int64_t stamp_us{0};
    uint64_t decoded{0};
    uint64_t decode_err{0};
    uint64_t prints{0};
    uint64_t stale{0};
    uint64_t crossed{0};
    uint64_t rebuilds{0};
    uint32_t books{0};
    uint32_t print_n{0};
    uint32_t fan_sinks{0};
    uint32_t pad{0};
    MarketBook book[kMarketBooks]{};
    MarketPrint print[kMarketPrints]{};
};

class MarketBoard {
public:
    void publish(const MarketSnap& s) noexcept {
        const uint64_t v = seq_.load(std::memory_order_relaxed);
        seq_.store(v + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        snap_ = s;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(v + 2, std::memory_order_release);
    }

    bool read(MarketSnap& out) const noexcept {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const uint64_t before = seq_.load(std::memory_order_acquire);
            if ((before & 1u) != 0) continue;
            out = snap_;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == before) return true;
        }
        return false;
    }

    uint64_t version() const noexcept { return seq_.load(std::memory_order_acquire); }

private:
    alignas(64) std::atomic<uint64_t> seq_{0};
    alignas(64) MarketSnap snap_{};
};

} // namespace pitlane
