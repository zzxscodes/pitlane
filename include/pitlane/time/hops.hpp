#pragma once

// 逐跳延迟账本：从网卡收包到报单发出的每一段单独计时，端到端再单独计时。
// 每段一个幅值直方图（time/mag.hpp），另记超预算次数，便于把一次提交的延迟变化
// 定位到具体跳。这里只接收纳秒，TSC 换算由调用方用 time/cycle.hpp 完成。
//
// 单写者、定长、无堆。HTTP 线程通过 report() 只读快照。

#include "pitlane/time/mag.hpp"

#include <cstdint>

namespace pitlane {

enum HopStage : uint8_t {
    kHopWireIn = 0, // 网卡收包 → 进程可见
    kHopDecode = 1, // 解码 / 校验
    kHopBook = 2,   // 订单簿或行情快照更新
    kHopSignal = 3, // 策略信号
    kHopRisk = 4,   // 风控与报单前检查
    kHopEncode = 5, // 报单编码
    kHopWireOut = 6,// 发送到网卡
    kHopAck = 7,    // 回报处理
    kHopCount = 8
};

inline constexpr int kHopStages = static_cast<int>(HopStage::kHopCount);

inline const char* hop_name(uint8_t stage) noexcept {
    switch (stage) {
        case kHopWireIn: return "wire_in";
        case kHopDecode: return "decode";
        case kHopBook: return "book";
        case kHopSignal: return "signal";
        case kHopRisk: return "risk";
        case kHopEncode: return "encode";
        case kHopWireOut: return "wire_out";
        case kHopAck: return "ack";
        default: return "total";
    }
}

struct HopStageReport {
    uint8_t stage{0};
    uint8_t pad[3]{};
    uint64_t n{0};
    uint64_t min_ns{0};
    uint64_t p50_ns{0};
    uint64_t p99_ns{0};
    uint64_t max_ns{0};
    uint64_t over{0}; // 超过该跳预算的次数
};

class HopLedger {
public:
    void reset() noexcept {
        for (int i = 0; i < kHopStages; ++i) {
            stage_[i] = MagHist{};
            over_[i] = 0;
        }
        total_ = MagHist{};
        total_over_ = 0;
    }

    // 每跳预算（纳秒），0 表示该跳不设预算；total 同理。
    void set_budget(uint8_t stage, uint64_t ns) noexcept {
        if (stage < kHopStages) budget_[stage] = ns;
    }
    void set_total_budget(uint64_t ns) noexcept { total_budget_ = ns; }

    // 记录一跳。stage 越界返回 false。
    bool note(uint8_t stage, uint64_t ns) noexcept {
        if (stage >= kHopStages) return false;
        if (!stage_[stage].add(ns)) return false;
        if (budget_[stage] != 0 && ns > budget_[stage]) ++over_[stage];
        return true;
    }

    bool note_total(uint64_t ns) noexcept {
        if (!total_.add(ns)) return false;
        if (total_budget_ != 0 && ns > total_budget_) ++total_over_;
        return true;
    }

    bool report(uint8_t stage, HopStageReport& out) const noexcept {
        if (stage >= kHopStages) return false;
        LatReport r{};
        if (!mag_report(stage_[stage], r)) return false;
        out.stage = stage;
        out.n = r.n;
        out.min_ns = r.min;
        out.p50_ns = r.p50;
        out.p99_ns = r.p99;
        out.max_ns = r.max;
        out.over = over_[stage];
        return true;
    }

    bool report_total(HopStageReport& out) const noexcept {
        LatReport r{};
        if (!mag_report(total_, r)) return false;
        out.stage = kHopCount;
        out.n = r.n;
        out.min_ns = r.min;
        out.p50_ns = r.p50;
        out.p99_ns = r.p99;
        out.max_ns = r.max;
        out.over = total_over_;
        return true;
    }

    uint64_t over(uint8_t stage) const noexcept { return stage < kHopStages ? over_[stage] : 0; }
    uint64_t total_over() const noexcept { return total_over_; }

private:
    MagHist stage_[kHopStages]{};
    uint64_t over_[kHopStages]{};
    uint64_t budget_[kHopStages]{};
    MagHist total_{};
    uint64_t total_over_{0};
    uint64_t total_budget_{0};
};

// 一条报单/行情的定长时间戳轨迹，用于离线复核某一笔为什么慢。
template <int N = kHopStages>
struct HopTrace {
    uint64_t ts_ns[N]{};
    uint8_t set_[N]{};
};

template <int N>
inline bool hop_trace_mark(HopTrace<N>& t, uint8_t stage, uint64_t now_ns) noexcept {
    if (stage >= N) return false;
    t.ts_ns[stage] = now_ns;
    t.set_[stage] = 1;
    return true;
}

// 把轨迹中相邻两跳的差值记入账本；缺失的跳被跳过。
template <int N>
inline int hop_trace_flush(HopTrace<N>& t, HopLedger& led, uint8_t from, uint8_t to) noexcept {
    int n = 0;
    for (int i = static_cast<int>(from) + 1; i <= static_cast<int>(to) && i < N; ++i) {
        if (!t.set_[i] || !t.set_[i - 1]) continue;
        if (t.ts_ns[i] < t.ts_ns[i - 1]) continue;
        if (led.note(static_cast<uint8_t>(i), t.ts_ns[i] - t.ts_ns[i - 1])) ++n;
    }
    if (from < N && to < N && t.set_[from] && t.set_[to] && t.ts_ns[to] >= t.ts_ns[from]) {
        led.note_total(t.ts_ns[to] - t.ts_ns[from]);
    }
    return n;
}

} // namespace pitlane
