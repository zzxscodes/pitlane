#pragma once

// 策略灰度：同一份配置下按 tag 的稳定哈希把一小部分流量路由到新版本，
// 用拒绝率、滑点和最小样本数三个闸门决定晋级还是回滚。
//
// 灰度桶按 tag 哈希固定，同一笔订单在重放时仍然落在同一侧，保证可复算。

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

enum CanaryState : uint8_t {
    kCanaryOff = 0,
    kCanaryLive = 1,
    kCanaryPromoted = 2,
    kCanaryRolledBack = 3
};

inline const char* canary_state_name(uint8_t s) noexcept {
    switch (s) {
        case kCanaryLive: return "live";
        case kCanaryPromoted: return "promoted";
        case kCanaryRolledBack: return "rolled_back";
        default: return "off";
    }
}

struct CanaryPlan {
    uint32_t id{0};
    char version[16]{};
    int32_t pct_ppm{100000};   // 10% 流量
    int32_t min_samples{100};
    int32_t max_reject_ppm{50000}; // 5% 拒绝率上限
    int64_t max_slip{1};           // 平均滑点上限（价格单位）
};

struct CanaryReport {
    uint8_t state{kCanaryOff};
    uint8_t gate_ok{0};
    uint16_t pad{0};
    uint64_t routed{0};
    uint64_t ok{0};
    uint64_t rejects{0};
    int64_t slip_sum{0};
    int32_t reject_ppm{0};
    int64_t avg_slip{0};
    CanaryPlan plan{};
};

class Canary {
public:
    void clear() noexcept {
        plan_ = CanaryPlan{};
        state_ = kCanaryOff;
        canary_ = Bucket{};
        control_ = Bucket{};
        report_ = CanaryReport{};
    }

    bool plan(const CanaryPlan& p) noexcept {
        if (p.pct_ppm <= 0 || p.pct_ppm > 1000000) return false;
        plan_ = p;
        report_ = CanaryReport{};
        report_.plan = p;
        report_.state = kCanaryLive;
        state_ = kCanaryLive;
        control_ = Bucket{};
        return true;
    }

    uint8_t state() const noexcept { return state_; }
    const CanaryPlan& config() const noexcept { return plan_; }

    // 该 tag 是否走灰度。非 live 状态一律返回 false（回滚后全部回旧版本）。
    bool pick(uint64_t tag) const noexcept {
        if (state_ != kCanaryLive && state_ != kCanaryPromoted) return false;
        if (state_ == kCanaryPromoted) return true;
        return static_cast<int32_t>(hash_tag(tag) % 1000000u) < plan_.pct_ppm;
    }

    void note(bool canary, bool ok, int64_t slip) noexcept {
        Bucket& b = canary ? canary_ : control_;
        ++b.routed;
        if (ok) ++b.ok;
        else ++b.rejects;
        if (slip > 0) b.slip_sum += slip;
        refresh();
    }

    // 闸门：样本足够、拒绝率不超限、平均滑点不超限。
    bool evaluate() const noexcept {
        if (state_ != kCanaryLive) return state_ == kCanaryPromoted;
        if (report_.routed < static_cast<uint64_t>(plan_.min_samples)) return false;
        return report_.reject_ppm <= plan_.max_reject_ppm && report_.avg_slip <= plan_.max_slip;
    }

    bool promote() noexcept {
        if (state_ != kCanaryLive || !evaluate()) return false;
        state_ = kCanaryPromoted;
        report_.state = state_;
        return true;
    }

    bool rollback() noexcept {
        if (state_ == kCanaryOff) return false;
        state_ = kCanaryRolledBack;
        report_.state = state_;
        return true;
    }

    CanaryReport report() const noexcept { return report_; }

private:
    struct Bucket {
        uint64_t routed{0};
        uint64_t ok{0};
        uint64_t rejects{0};
        int64_t slip_sum{0};
    };

    static uint64_t hash_tag(uint64_t x) noexcept {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33;
        return x;
    }

    void refresh() noexcept {
        report_.routed = canary_.routed;
        report_.ok = canary_.ok;
        report_.rejects = canary_.rejects;
        report_.slip_sum = canary_.slip_sum;
        const uint64_t n = canary_.routed;
        report_.reject_ppm = n > 0 ? static_cast<int32_t>(canary_.rejects * 1000000ull / n) : 0;
        report_.avg_slip = n > 0 ? canary_.slip_sum / static_cast<int64_t>(n) : 0;
        report_.gate_ok = evaluate() ? 1 : 0;
        report_.state = state_;
    }

    CanaryPlan plan_{};
    CanaryReport report_{};
    Bucket canary_{};
    Bucket control_{};
    uint8_t state_{kCanaryOff};
};

} // namespace pitlane
