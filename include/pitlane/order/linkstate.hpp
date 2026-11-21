#pragma once

// 报单链路状态机：报单、回报、撤单三条路径共用一套在途跟踪与超时判定。
// 目标是把“链路是否可信”变成一个明确的状态，而不是靠日志猜测：
//   Down → Up → (序号缺口 / 回报超时 / 主动下线) → Recovering → Up
// 处于 Recovering 时调用方必须重发或对账，链路不会自动假装健康。
//
// 定长、无堆、无锁。单线程调用，时间由调用方传入，方便回放与测试。

#include "pitlane/time/mag.hpp"

#include <cstdint>

namespace pitlane {

enum LinkPhase : uint8_t {
    kLinkDown = 0,
    kLinkUp = 1,
    kLinkDegraded = 2,   // 有在途未回报或收到过拒绝，仍可继续但需关注
    kLinkRecovering = 3  // 序号缺口或超时后正在对账/重发
};

inline const char* link_phase_name(uint8_t p) noexcept {
    switch (p) {
        case kLinkUp: return "up";
        case kLinkDegraded: return "degraded";
        case kLinkRecovering: return "recovering";
        default: return "down";
    }
}

struct LinkCfg {
    int64_t hb_timeout_us{1000000};     // 没有任何回报多久算链路可疑
    int64_t ack_timeout_us{300000};     // 报单在途多久算迟回报
    int64_t cancel_timeout_us{300000};  // 撤单在途多久算迟撤单
    int32_t max_inflight{512};
    int32_t degrade_reject_ppm{10000};  // 拒绝率超过 1% 进入 Degraded
};

struct LinkStats {
    uint64_t orders{0};
    uint64_t acks{0};
    uint64_t late_acks{0};
    uint64_t rejects{0};
    uint64_t cancels{0};
    uint64_t cancel_acks{0};
    uint64_t late_cancels{0};
    uint64_t gaps{0};
    uint64_t drops{0};      // 链路 Down 或重复 tag 被拒
    uint64_t recoveries{0};
    uint64_t seq{0};
};

class OrderLink {
public:
    static constexpr int kSlots = 2048;

    OrderLink() noexcept { reset(); }

    void reset() noexcept {
        for (int i = 0; i < kSlots; ++i) {
            key_[i] = 0;
            slot_[i] = -1;
            state_[i] = 0;
        }
        for (int i = 0; i < kPending; ++i) {
            pending_[i] = Pending{};
            pending_[i].next = (i + 1 < kPending) ? i + 1 : -1;
        }
        free_ = 0;
        inflight_ = 0;
        phase_ = kLinkDown;
        began_us_ = 0;
        last_rx_us_ = 0;
        resync_gen_ = 0;
        next_seq_ = 0;
        seq_started_ = 0;
        stats_ = LinkStats{};
        hist_ack_ = MagHist{};
        hist_cancel_ = MagHist{};
    }

    const LinkCfg& cfg() const noexcept { return cfg_; }
    void set_cfg(const LinkCfg& c) noexcept { cfg_ = c; }

    void up(int64_t now_us) noexcept {
        phase_ = kLinkUp;
        began_us_ = now_us;
        last_rx_us_ = now_us;
        seq_started_ = 0;
        clear_pending();
    }

    // 主动或异常下线：清空在途，要求调用方重连后重新对账。
    void down(int64_t now_us) noexcept {
        phase_ = kLinkDown;
        began_us_ = now_us;
        clear_pending();
        ++resync_gen_;
    }

    uint8_t phase() const noexcept { return phase_; }
    uint64_t resync_gen() const noexcept { return resync_gen_; }
    int64_t inflight() const noexcept { return inflight_; }
    const LinkStats& stats() const noexcept { return stats_; }

    // 报单入在途表。链路 Down 或重复 tag 返回 false。
    bool note_order(uint64_t tag, int64_t now_us) noexcept {
        ++stats_.orders;
        if (phase_ == kLinkDown || tag == 0) {
            ++stats_.drops;
            return false;
        }
        if (find(tag) >= 0) {
            ++stats_.drops;
            return false;
        }
        if (inflight_ >= cfg_.max_inflight || free_ < 0) {
            ++stats_.drops;
            return false;
        }
        const int i = free_;
        Pending& p = pending_[i];
        free_ = p.next;
        p = Pending{};
        p.tag = tag;
        p.sent_us = now_us;
        p.kind = 1;
        p.used = 1;
        p.next = -1;
        ++inflight_;
        put(tag, i);
        return true;
    }

    // 报单回报。找到在途记录时记录 RTT；迟到由 tick 判定，这里只做解除。
    bool note_ack(uint64_t tag, int64_t now_us, bool rejected = false) noexcept {
        ++stats_.acks;
        if (rejected) ++stats_.rejects;
        last_rx_us_ = now_us;
        const int i = find(tag);
        if (i < 0) return false;
        Pending& p = pending_[i];
        if (p.kind == 2) return false;
        const int64_t rtt = now_us >= p.sent_us ? now_us - p.sent_us : 0;
        (void)hist_ack_.add(static_cast<uint64_t>(rtt));
        release(i);
        if (rejected && reject_rate_ppm() > cfg_.degrade_reject_ppm && phase_ == kLinkUp) {
            phase_ = kLinkDegraded;
        }
        return true;
    }

    bool note_cancel(uint64_t tag, int64_t now_us) noexcept {
        ++stats_.cancels;
        if (phase_ == kLinkDown || tag == 0) return false;
        const int i = find(tag);
        if (i < 0) return false;
        pending_[i].kind = 2;
        pending_[i].cancel_us = now_us;
        return true;
    }

    bool note_cancel_ack(uint64_t tag, int64_t now_us) noexcept {
        ++stats_.cancel_acks;
        last_rx_us_ = now_us;
        const int i = find(tag);
        if (i < 0) return false;
        const Pending& p = pending_[i];
        if (p.kind == 2 && p.cancel_us > 0) {
            const int64_t rtt = now_us >= p.cancel_us ? now_us - p.cancel_us : 0;
            (void)hist_cancel_.add(static_cast<uint64_t>(rtt));
        }
        release(i);
        return true;
    }

    // 入站序号监测。缺口进入 Recovering，并要求新的重发代次。
    bool note_seq(uint32_t seq) noexcept {
        if (!seq_started_) {
            seq_started_ = 1;
            next_seq_ = seq + 1u;
            stats_.seq = seq;
            return true;
        }
        if (seq == next_seq_) {
            ++next_seq_;
            stats_.seq = seq;
            return true;
        }
        if (seq > next_seq_) {
            ++stats_.gaps;
            ++resync_gen_;
            phase_ = kLinkRecovering;
        }
        // 旧序号静默忽略，不把重放当成新数据。
        next_seq_ = seq + 1u;
        stats_.seq = seq;
        return false;
    }

    // 周期性巡检：在途超时、心跳超时。返回本次新进入 Recovering 的次数。
    int tick(int64_t now_us) noexcept {
        if (phase_ == kLinkDown) return 0;
        int new_recover = 0;
        for (int i = 0; i < kPending; ++i) {
            Pending& p = pending_[i];
            if (!p.used) continue;
            if (p.kind == 2) {
                const int64_t base = p.cancel_us > 0 ? p.cancel_us : p.sent_us;
                if (now_us - base > cfg_.cancel_timeout_us) {
                    ++stats_.late_cancels;
                    release(i);
                    if (phase_ != kLinkRecovering) {
                        phase_ = kLinkRecovering;
                        ++resync_gen_;
                        ++stats_.recoveries;
                        ++new_recover;
                    }
                }
            } else if (now_us - p.sent_us > cfg_.ack_timeout_us) {
                ++stats_.late_acks;
                release(i);
                if (phase_ != kLinkRecovering) {
                    phase_ = kLinkRecovering;
                    ++resync_gen_;
                    ++stats_.recoveries;
                    ++new_recover;
                }
            }
        }
        if (phase_ == kLinkUp && last_rx_us_ > 0 && now_us - last_rx_us_ > cfg_.hb_timeout_us) {
            phase_ = kLinkDegraded;
        }
        return new_recover;
    }

    // 对账完成：在途清空且入站连续后恢复 Up。由调用方决定何时调用。
    bool recovered(int64_t now_us) noexcept {
        if (phase_ != kLinkRecovering) return false;
        if (inflight_ != 0) return false;
        phase_ = kLinkUp;
        last_rx_us_ = now_us;
        return true;
    }

    int64_t ack_p50() const noexcept {
        LatReport r{};
        return mag_report(hist_ack_, r) ? static_cast<int64_t>(r.p50) : 0;
    }
    int64_t ack_p99() const noexcept {
        LatReport r{};
        return mag_report(hist_ack_, r) ? static_cast<int64_t>(r.p99) : 0;
    }
    int64_t cancel_p99() const noexcept {
        LatReport r{};
        return mag_report(hist_cancel_, r) ? static_cast<int64_t>(r.p99) : 0;
    }

private:
    struct Pending {
        uint64_t tag{0};
        int64_t sent_us{0};
        int64_t cancel_us{0};
        int32_t next{-1};
        uint8_t kind{0}; // 1 报单，2 撤单
        uint8_t used{0};
        uint8_t pad[2]{};
    };

    static constexpr int kPending = 1024;

    static uint32_t hash_tag(uint64_t x) noexcept {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33;
        return static_cast<uint32_t>(x);
    }

    int find(uint64_t tag) const noexcept {
        if (tag == 0) return -1;
        uint32_t h = hash_tag(tag) & (kSlots - 1u);
        for (int probe = 0; probe < kSlots; ++probe) {
            if (state_[h] == 0) return -1;
            if (state_[h] == 1 && key_[h] == tag && pending_[slot_[h]].used) return slot_[h];
            h = (h + 1) & (kSlots - 1u);
        }
        return -1;
    }

    void put(uint64_t tag, int slot) noexcept {
        uint32_t h = hash_tag(tag) & (kSlots - 1u);
        uint32_t tomb = UINT32_MAX;
        for (int probe = 0; probe < kSlots; ++probe) {
            if (state_[h] == 0) {
                if (tomb != UINT32_MAX) h = tomb;
                key_[h] = tag;
                slot_[h] = slot;
                state_[h] = 1;
                return;
            }
            if (state_[h] == 1 && key_[h] == tag) {
                slot_[h] = slot;
                return;
            }
            if (state_[h] == 2 && tomb == UINT32_MAX) tomb = h;
            h = (h + 1) & (kSlots - 1u);
        }
    }

    void drop_key(uint64_t tag) noexcept {
        uint32_t h = hash_tag(tag) & (kSlots - 1u);
        for (int probe = 0; probe < kSlots; ++probe) {
            if (state_[h] == 0) return;
            if (state_[h] == 1 && key_[h] == tag) {
                state_[h] = 2;
                slot_[h] = -1;
                return;
            }
            h = (h + 1) & (kSlots - 1u);
        }
    }

    void release(int i) noexcept {
        Pending& p = pending_[i];
        if (!p.used) return;
        drop_key(p.tag);
        p.used = 0;
        p.next = free_;
        free_ = i;
        if (inflight_ > 0) --inflight_;
    }

    void clear_pending() noexcept {
        for (int i = 0; i < kPending; ++i) {
            pending_[i] = Pending{};
            pending_[i].next = (i + 1 < kPending) ? i + 1 : -1;
        }
        free_ = 0;
        inflight_ = 0;
        for (int i = 0; i < kSlots; ++i) {
            key_[i] = 0;
            slot_[i] = -1;
            state_[i] = 0;
        }
    }

    int32_t reject_rate_ppm() const noexcept {
        return stats_.acks > 0 ? static_cast<int32_t>(stats_.rejects * 1000000ull / stats_.acks) : 0;
    }

    LinkCfg cfg_{};
    LinkStats stats_{};
    Pending pending_[kPending]{};
    uint64_t key_[kSlots]{};
    int32_t slot_[kSlots]{};
    uint8_t state_[kSlots]{};
    MagHist hist_ack_{};
    MagHist hist_cancel_{};
    int free_{-1};
    int inflight_{0};
    uint8_t phase_{kLinkDown};
    uint8_t seq_started_{0};
    int64_t began_us_{0};
    int64_t last_rx_us_{0};
    uint64_t resync_gen_{0};
    uint32_t next_seq_{0};
};

} // namespace pitlane
