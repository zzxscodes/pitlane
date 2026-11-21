#pragma once

// CTP 行情解码内核：把行情回调的原始字段（double 价格、HH:MM:SS 时间、快照量）
// 转成定点整数，并重建逐笔成交。这里只接收原始字段，适配层只负责搬运。
//
// 价格统一放大 1e4，数量为整数手/张；所有函数不使用堆、不抛异常、不访问系统时钟。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

inline constexpr int kCtpLevels = 5;

// 交易所给出的原始快照。无效价格用 DBL_MAX 或 0 表示，和 CTP 一致。
struct CtpRaw {
    char instrument[32]{};
    char exchange[16]{};
    char trading_day[9]{};
    char action_day[9]{};
    char update_time[9]{};
    int32_t update_ms{0};
    double last_px{0};
    double pre_settle{0};
    double pre_close{0};
    double open_px{0};
    double high_px{0};
    double low_px{0};
    double volume{0};
    double turnover{0};
    double open_interest{0};
    double upper_limit{0};
    double lower_limit{0};
    double bid_px[kCtpLevels]{};
    double bid_qty[kCtpLevels]{};
    double ask_px[kCtpLevels]{};
    double ask_qty[kCtpLevels]{};
};

struct CtpLevel {
    int64_t px{0};
    int64_t qty{0};
};

enum CtpStatus : uint8_t {
    kCtpNoQuote = 0,   // 无有效买卖价（未开盘、停牌或集合竞价前）
    kCtpTrading = 1,   // 正常连续竞价
    kCtpLimitUp = 2,   // 最新价触及涨停
    kCtpLimitDown = 3, // 最新价触及跌停
    kCtpAuction = 4    // 有报价但最新价为 0，视为集合竞价阶段
};

struct CtpSnap {
    uint32_t name{0};
    uint32_t seq{0};
    uint8_t status{kCtpNoQuote};
    uint8_t n_bid{0};
    uint8_t n_ask{0};
    uint8_t crossed{0};
    int64_t exch_us{0};
    int64_t local_us{0};
    int64_t last_px{0};
    int64_t last_qty{0};
    int64_t volume{0};
    int64_t turnover{0};
    int64_t open_interest{0};
    int64_t pre_close{0};
    int64_t open_px{0};
    int64_t high_px{0};
    int64_t low_px{0};
    int64_t upper_limit{0};
    int64_t lower_limit{0};
    CtpLevel bid[kCtpLevels]{};
    CtpLevel ask[kCtpLevels]{};
};

// 逐笔成交。由相邻两次快照的累计量差重建，一条 delta 可能合并多笔真实成交。
struct CtpPrint {
    uint32_t name{0};
    int64_t exch_us{0};
    int64_t px{0};
    int64_t qty{0};
    int64_t volume{0};   // 累计成交量
    int64_t turnover{0}; // 累计成交额（元）
    uint8_t aggressor{0}; // 1 买主动，2 卖主动，0 无法判定
    uint8_t last{0};      // 1 表示该品种当日最后一条快照
};

inline bool ctp_valid_px(double x) noexcept {
    return x > 0.0 && x < 1e15;
}

// 价位放大 1e4 后四舍五入。无效值返回 0。
inline int64_t ctp_px(double x) noexcept {
    if (!ctp_valid_px(x)) return 0;
    return static_cast<int64_t>(x * 10000.0 + 0.5);
}

// HH:MM:SS + 毫秒 → 当日微秒。空串或非法值返回 0。
inline int64_t ctp_time_us(const char* hhmmss, int ms) noexcept {
    if (!hhmmss) return 0;
    int h = 0, m = 0, s = 0;
    if (std::strlen(hhmmss) >= 8) {
        h = (hhmmss[0] - '0') * 10 + (hhmmss[1] - '0');
        m = (hhmmss[3] - '0') * 10 + (hhmmss[4] - '0');
        s = (hhmmss[6] - '0') * 10 + (hhmmss[7] - '0');
    }
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return 0;
    if (ms < 0) ms = 0;
    if (ms > 999) ms = 999;
    return (static_cast<int64_t>(h) * 3600 + m * 60 + s) * 1000000ll + static_cast<int64_t>(ms) * 1000ll;
}

inline const char* ctp_status_name(uint8_t s) noexcept {
    switch (s) {
        case kCtpTrading: return "trading";
        case kCtpLimitUp: return "limit_up";
        case kCtpLimitDown: return "limit_down";
        case kCtpAuction: return "auction";
        default: return "no_quote";
    }
}

// 品种代码 → 稳定编号（FNV-1a 32 位，跳过 0）。仅用于内核内部索引；
// 与 InstMap 对齐时由适配层覆盖为正式编号。
inline uint32_t ctp_name_of(const char* instrument) noexcept {
    uint32_t h = 2166136261u;
    for (const char* p = instrument; p && *p; ++p) {
        h ^= static_cast<uint8_t>(*p);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

// 把原始快照解码成定点的 CtpSnap。时间无效或品种为空时返回 false。
// name 取品种代码的稳定哈希；需要与 InstMap 对齐时由调用方覆盖。
// 档位按价格从优到劣排列，无效档位被跳过；买卖价交叉只做标记，不修改数据。
inline bool ctp_decode(const CtpRaw& raw, CtpSnap& out) noexcept {
    if (!raw.instrument[0]) return false;
    const int64_t exch = ctp_time_us(raw.update_time, raw.update_ms);
    if (exch <= 0) return false;

    // 直接写调用方的快照，省掉整结构零初始化与一次整体拷贝。未用到的档位
    // 仍补零，保持“n 之外的档位恒为零”的既有约定。
    out.name = ctp_name_of(raw.instrument);
    out.seq = 0;
    out.local_us = 0;
    out.last_qty = 0;
    out.exch_us = exch;
    out.last_px = ctp_px(raw.last_px);
    out.pre_close = ctp_px(raw.pre_close);
    out.open_px = ctp_px(raw.open_px);
    out.high_px = ctp_px(raw.high_px);
    out.low_px = ctp_px(raw.low_px);
    out.upper_limit = ctp_px(raw.upper_limit);
    out.lower_limit = ctp_px(raw.lower_limit);
    out.volume = raw.volume > 0 ? static_cast<int64_t>(raw.volume) : 0;
    out.turnover = raw.turnover > 0 ? static_cast<int64_t>(raw.turnover) : 0;
    out.open_interest = raw.open_interest > 0 ? static_cast<int64_t>(raw.open_interest) : 0;

    out.n_bid = 0;
    out.n_ask = 0;
    for (int i = 0; i < kCtpLevels; ++i) {
        const int64_t bp = ctp_px(raw.bid_px[i]);
        const int64_t ap = ctp_px(raw.ask_px[i]);
        const int64_t bq = raw.bid_qty[i] > 0 ? static_cast<int64_t>(raw.bid_qty[i]) : 0;
        const int64_t aq = raw.ask_qty[i] > 0 ? static_cast<int64_t>(raw.ask_qty[i]) : 0;
        if (bp > 0 && bq > 0 && out.n_bid < kCtpLevels) out.bid[out.n_bid++] = CtpLevel{bp, bq};
        if (ap > 0 && aq > 0 && out.n_ask < kCtpLevels) out.ask[out.n_ask++] = CtpLevel{ap, aq};
    }
    // 只在无效档位处跳过的写入方式可能让尾部残留上一次的旧值，这里补齐零值，
    // 保持“n 之外的档位恒为零”的既有约定。
    for (int i = out.n_bid; i < kCtpLevels; ++i) out.bid[i] = CtpLevel{};
    for (int i = out.n_ask; i < kCtpLevels; ++i) out.ask[i] = CtpLevel{};

    const int64_t bid1 = out.n_bid > 0 ? out.bid[0].px : 0;
    const int64_t ask1 = out.n_ask > 0 ? out.ask[0].px : 0;
    out.crossed = bid1 > 0 && ask1 > 0 && bid1 >= ask1 ? 1 : 0;

    if (out.n_bid == 0 && out.n_ask == 0) {
        out.status = kCtpNoQuote;
    } else if (out.last_px <= 0) {
        out.status = kCtpAuction;
    } else if (out.upper_limit > 0 && out.last_px >= out.upper_limit) {
        out.status = kCtpLimitUp;
    } else if (out.lower_limit > 0 && out.last_px <= out.lower_limit) {
        out.status = kCtpLimitDown;
    } else {
        out.status = kCtpTrading;
    }
    return true;
}

// 逐笔重建器。累计成交量回退（夜盘换日、重连重放）时自动开新一段，
// 并给出 last 标记，避免把回退差值当成一笔巨量成交。
class CtpPrintMaker {
public:
    void reset() noexcept {
        started_ = 0;
        day_ = 0;
        volume_ = 0;
        turnover_ = 0;
        last_px_ = 0;
        seq_ = 0;
    }

    int64_t volume() const noexcept { return volume_; }

    // 返回写入 out 的条数（0 或 1）。out 为空时只更新内部状态。
    int on_snap(const CtpSnap& s, CtpPrint* out, int cap) noexcept {
        if (out == nullptr || cap <= 0) {
            observe(s);
            return 0;
        }
        const int64_t dv = delta(s);
        const int64_t dt = s.turnover - turnover_;
        if (dv < 0) {
            // 累计量回退：换日或重放。吞掉这一段，不产出成交。
            reset();
            started_ = 1;
            volume_ = s.volume;
            turnover_ = s.turnover;
            last_px_ = s.last_px;
            return 0;
        }
        started_ = 1;
        volume_ = s.volume;
        turnover_ = s.turnover;
        if (dv == 0) {
            last_px_ = s.last_px > 0 ? s.last_px : last_px_;
            return 0;
        }

        CtpPrint p{};
        p.name = s.name;
        p.exch_us = s.exch_us;
        p.volume = s.volume;
        p.turnover = s.turnover;
        p.qty = dv;
        // 成交额差能给出更接近成交均价的价位；缺失或明显偏离时退回最新价。
        int64_t px = s.last_px;
        if (dt > 0 && dv > 0) {
            const int64_t avg = dt * 10000 / dv;
            const int64_t lo = s.low_px > 0 ? s.low_px : 0;
            const int64_t hi = s.high_px > 0 ? s.high_px : 0;
            if (avg > 0 && (lo == 0 || avg >= lo) && (hi == 0 || avg <= hi)) px = avg;
        }
        p.px = px;
        p.aggressor = aggressor_of(s, px, last_px_);
        p.last = 0;
        last_px_ = px > 0 ? px : last_px_;
        ++seq_;
        out[0] = p;
        return 1;
    }

    uint32_t seq() const noexcept { return seq_; }

private:
    void observe(const CtpSnap& s) noexcept {
        if (!started_) {
            started_ = 1;
            volume_ = s.volume;
            turnover_ = s.turnover;
            last_px_ = s.last_px;
            return;
        }
        if (s.volume < volume_) reset();
        volume_ = s.volume;
        turnover_ = s.turnover;
        if (s.last_px > 0) last_px_ = s.last_px;
    }

    int64_t delta(const CtpSnap& s) const noexcept {
        if (!started_) return 0;
        return s.volume - volume_;
    }

    static uint8_t aggressor_of(const CtpSnap& s, int64_t px, int64_t prev_px) noexcept {
        const int64_t bid1 = s.n_bid > 0 ? s.bid[0].px : 0;
        const int64_t ask1 = s.n_ask > 0 ? s.ask[0].px : 0;
        // 方向优先看交易所最新价：它才是主动成交打到的价；成交均价只用于价位。
        const int64_t ref = s.last_px > 0 ? s.last_px : px;
        if (ref > 0 && ask1 > 0 && ref >= ask1) return 1;
        if (ref > 0 && bid1 > 0 && ref <= bid1) return 2;
        if (prev_px > 0 && ref > prev_px) return 1;
        if (prev_px > 0 && ref < prev_px) return 2;
        return 0;
    }

    uint8_t started_{0};
    uint8_t day_{0};
    int64_t volume_{0};
    int64_t turnover_{0};
    int64_t last_px_{0};
    uint32_t seq_{0};
};

} // namespace pitlane
