#pragma once

// 配对交易内核：两条腿的价差均值回归策略。
//
// * 价差 = A 中间价 - ratio * B 中间价，ratio 用 ppm 表示，全程整数，无浮点。
// * 固定窗口滚动均值与方差，z 值放大到 ppm；样本不足或任一腿行情过期时不出信号。
// * 开仓 |z| >= entry，平仓 |z| <= exit，止损 |z| >= stop，超时强平，冷却期内不再开仓。
// * 每条腿的目标数量由 A 腿手数和 ratio 推出；未完成的差额可以重复补发，由调用方限速。
// * 成交按成本记入各腿，双腿回到零仓时结算已实现盈亏并扣除手续费 ppm。
// * 固定容量、无堆、无锁；行情线程只写最新盘口，交易线程推进状态机。

#include <cstdint>

namespace pitlane {

enum : uint8_t {
    kPairFlat = 0,
    kPairOpening = 1,
    kPairHolding = 2,
    kPairClosing = 3,
    kPairStopped = 4
};

inline const char* pair_state_name(uint8_t s) noexcept {
    switch (s) {
        case kPairOpening: return "opening";
        case kPairHolding: return "holding";
        case kPairClosing: return "closing";
        case kPairStopped: return "stopped";
        default: return "flat";
    }
}

struct PairCfg {
    uint32_t name_a{1};
    uint32_t name_b{2};
    uint32_t acct{1};
    // B 腿数量 = A 腿数量 * ratio_ppm / 1e6，至少 1 手。
    int64_t ratio_ppm{1000000};
    int64_t qty_a{1};
    uint16_t window{32};
    int64_t entry_z_ppm{2000000};
    int64_t exit_z_ppm{500000};
    int64_t stop_z_ppm{4000000};
    int64_t max_hold_us{60000000};
    int64_t leg_timeout_us{2000000};
    int64_t cooldown_us{1000000};
    int64_t max_stale_us{5000000};
    // 两腿行情时间差上限；超过则说明一条腿的盘口已过期，本拍不更新价差样本。
    int64_t max_skew_us{0};  // 0 表示不检查
    int64_t max_rounds{0};  // 0 表示不限
    int64_t fee_ppm{0};
    // 主动腿吃对价，被动腿按 pending_ppm 从本方最优价向对方最优价让价；
    // pending_ppm 为 0 时两腿都吃对价。sequential 先完成主动腿再发被动腿。
    uint8_t main_leg{0};
    int64_t pending_ppm{0};
    bool sequential{false};
    // 单腿单批上限，0 表示不限。
    int64_t max_leg_qty{0};
    // 被动腿在途价偏离最新目标价超过阈值时改价或撤单重报，0 表示关闭。
    int64_t pending_modify_off{0};
    int64_t pending_cancel_off{0};
    // 报单发出后迟迟没有回报的判定窗口，0 表示关闭。
    int64_t ack_timeout_us{0};
    // 入场前用对价复核组合价差是否仍在入场带之外。
    bool combo_check{false};
};

// 被动腿在途单的处置动作。
enum : uint8_t {
    kPendingKeep = 0,
    kPendingReprice = 1,
    kPendingCancel = 2
};

struct PairTick {
    int64_t bid{0};
    int64_t ask{0};
    int64_t ts_us{0};
};

// 一条待发指令。side 1 买、2 卖；leg 0 是 A、1 是 B。
struct PairReq {
    uint32_t name{0};
    uint8_t side{0};
    uint8_t leg{0};
    int64_t qty{0};
    int64_t px{0};
};

// 控制台和回放读取的一行快照。
struct PairRow {
    uint8_t state{kPairFlat};
    uint8_t ready{0};
    int8_t direction{0};
    int64_t spread{0};
    int64_t mean{0};
    int64_t sd{0};
    int64_t z_ppm{0};
    int64_t pos_a{0};
    int64_t pos_b{0};
    int64_t target_a{0};
    int64_t target_b{0};
    int64_t rounds{0};
    int64_t wins{0};
    int64_t losses{0};
    int64_t realized{0};
    int64_t signals{0};
    int64_t orders{0};
    int64_t rejects{0};
    int64_t timeouts{0};
    int64_t stop_hits{0};
    int64_t samples{0};
    int64_t last_sig_us{0};
    int64_t opened_us{0};
};

class PairEngine {
public:
    static constexpr uint16_t kMaxWindow = 256;
    static constexpr int kMaxReq = 2;

    bool open(const PairCfg& c) noexcept {
        if (c.name_a == 0 || c.name_b == 0 || c.name_a == c.name_b) return false;
        if (c.window < 2 || c.window > kMaxWindow) return false;
        if (c.ratio_ppm <= 0 || c.qty_a <= 0) return false;
        if (c.entry_z_ppm <= 0 || c.exit_z_ppm < 0 || c.stop_z_ppm < c.entry_z_ppm) return false;
        if (c.max_hold_us < 0 || c.leg_timeout_us <= 0 || c.cooldown_us < 0) return false;
        if (c.main_leg > 1 || c.pending_ppm < 0 || c.max_leg_qty < 0) return false;
        if (c.pending_modify_off < 0 || c.pending_cancel_off < 0 || c.ack_timeout_us < 0) return false;
        cfg_ = c;
        reset(0);
        open_ = true;
        return true;
    }

    bool is_open() const noexcept { return open_; }
    const PairCfg& cfg() const noexcept { return cfg_; }
    uint8_t state() const noexcept { return state_; }
    bool ready() const noexcept { return ready_; }
    bool paused() const noexcept { return paused_; }
    bool stopped() const noexcept { return state_ == kPairStopped; }

    // 暂停只是不再产生新指令，在途单和状态保持；恢复后从原状态继续。
    bool pause() noexcept {
        if (!open_ || paused_ || state_ == kPairStopped) return false;
        paused_ = true;
        return true;
    }

    bool resume() noexcept {
        if (!open_ || !paused_) return false;
        paused_ = false;
        return true;
    }

    // 停止后不再产生指令，目标清零。调用方负责撤销在途单并处理剩余持仓。
    bool stop() noexcept {
        if (!open_ || state_ == kPairStopped) return false;
        paused_ = false;
        state_ = kPairStopped;
        target_[0] = 0;
        target_[1] = 0;
        cancel_all();
        return true;
    }

    // 撤单回报：清掉该腿在途量，下一拍按真实持仓重新计算差额。
    void on_cancel(uint8_t leg) noexcept {
        if (leg > 1) return;
        pending_[leg] = 0;
        pend_live_[leg] = 0;
        pend_ts_[leg] = 0;
    }

    // 改价成功：更新被动腿在途价，避免下一拍重复改价。
    void on_amend(uint8_t leg, int64_t px) noexcept {
        if (leg > 1 || px <= 0) return;
        pend_px_[leg] = px;
    }

    void cancel_all() noexcept {
        pending_[0] = 0;
        pending_[1] = 0;
        pend_live_[0] = 0;
        pend_live_[1] = 0;
        pend_ts_[0] = 0;
        pend_ts_[1] = 0;
    }

    // 按对价估算的组合价差：dir > 0 买 A 卖 B，dir < 0 卖 A 买 B。
    int64_t combo_spread(int8_t dir) const noexcept {
        if (dir == 0) return 0;
        if (dir > 0) {
            if (ask_[0] <= 0 || bid_[1] <= 0) return 0;
            return ask_[0] - bid_[1] * cfg_.ratio_ppm / 1000000;
        }
        if (bid_[0] <= 0 || ask_[1] <= 0) return 0;
        return bid_[0] - ask_[1] * cfg_.ratio_ppm / 1000000;
    }

    // 组合价差是否仍在入场带之外，避免信号与下单之间盘口反向吞掉价差。
    bool combo_ok(int8_t dir) const noexcept {
        if (!cfg_.combo_check || sd_ <= 0 || dir == 0) return true;
        if (dir > 0) {
            if (ask_[0] <= 0 || bid_[1] <= 0) return false;
        } else if (bid_[0] <= 0 || ask_[1] <= 0) {
            return false;
        }
        const int64_t band = cfg_.entry_z_ppm * sd_ / 1000000;
        const int64_t combo = combo_spread(dir);
        if (dir > 0) return combo <= mean_ - band;
        return combo >= mean_ + band;
    }

    // 被动腿在途价相对最新目标价的处置；new_px 返回最新目标价。
    uint8_t pending_action(uint8_t leg, int64_t& new_px) const noexcept {
        new_px = 0;
        if (leg > 1 || !pend_live_[leg] || pending_[leg] == 0) return kPendingKeep;
        // 只有让价挂出的被动腿才有改撤价问题。
        if (cfg_.pending_ppm <= 0 || leg == cfg_.main_leg) return kPendingKeep;
        const bool buy = pending_[leg] > 0;
        const int64_t want = leg_px(leg, buy);
        if (want <= 0 || pend_px_[leg] <= 0) return kPendingKeep;
        new_px = want;
        const int64_t drift = want >= pend_px_[leg] ? want - pend_px_[leg] : pend_px_[leg] - want;
        if (cfg_.pending_cancel_off > 0 && drift >= cfg_.pending_cancel_off) return kPendingCancel;
        if (cfg_.pending_modify_off > 0 && drift >= cfg_.pending_modify_off) return kPendingReprice;
        return kPendingKeep;
    }

    // 该腿在途单是否已超过回报超时。
    bool unack_expired(uint8_t leg, int64_t now) const noexcept {
        if (leg > 1 || cfg_.ack_timeout_us <= 0 || !pend_live_[leg]) return false;
        return pend_ts_[leg] > 0 && now - pend_ts_[leg] >= cfg_.ack_timeout_us;
    }

    void reset(int64_t now) noexcept {
        state_ = kPairFlat;
        dir_ = 0;
        paused_ = false;
        for (int i = 0; i < 2; ++i) {
            mid_[i] = 0;
            bid_[i] = 0;
            ask_[i] = 0;
            ts_[i] = 0;
            pos_[i] = 0;
            target_[i] = 0;
            pending_[i] = 0;
            pend_px_[i] = 0;
            pend_ts_[i] = 0;
            pend_live_[i] = 0;
            cost_[i] = 0;
        }
        dirty_ = false;
        n_ = 0;
        head_ = 0;
        sum_ = 0;
        spread_ = 0;
        mean_ = 0;
        sd_ = 0;
        z_ppm_ = 0;
        ready_ = false;
        fees_ = 0;
        realized_ = 0;
        rounds_ = 0;
        wins_ = 0;
        losses_ = 0;
        signals_ = 0;
        orders_ = 0;
        rejects_ = 0;
        timeouts_ = 0;
        stop_hits_ = 0;
        last_sig_us_ = 0;
        last_exit_us_ = now;
        opened_us_ = 0;
        hold_from_ = 0;
        close_from_ = 0;
    }

    // 行情线程或交易线程：写入一条最新盘口。
    void on_quote(uint8_t leg, const PairTick& t) noexcept {
        if (leg > 1) return;
        int64_t mid = 0;
        if (t.bid > 0 && t.ask > 0 && t.ask >= t.bid) {
            mid = (t.bid + t.ask) / 2;
        } else {
            mid = t.ask > 0 ? t.ask : t.bid;
        }
        if (mid <= 0) return;
        bid_[leg] = t.bid;
        ask_[leg] = t.ask;
        mid_[leg] = mid;
        ts_[leg] = t.ts_us;
        dirty_ = true;
    }

    // 成交回报：leg 0/1，side 1 买、2 卖。
    void on_fill(uint8_t leg, uint8_t side, int64_t qty, int64_t px) noexcept {
        if (leg > 1 || (side != 1 && side != 2) || qty <= 0 || px <= 0) return;
        apply_fill(leg, side == 1 ? qty : -qty, px);
    }

    // 用最新持仓对账。只在持仓确实变化时按最新盘口补记成本，并清空该腿在途量。
    void sync_position(uint8_t leg, int64_t pos) noexcept {
        if (leg > 1) return;
        if (pos == pos_[leg]) return;
        const int64_t d = pos - pos_[leg];
        const int64_t px = d > 0 ? ask_[leg] : bid_[leg];
        if (px > 0) apply_fill(leg, d, px);
        else pos_[leg] = pos;
        pending_[leg] = 0;
    }

    void on_reject(uint8_t leg) noexcept {
        if (leg > 1) return;
        ++rejects_;
        pending_[leg] = 0;
    }

    // 计算价差与 z 值。返回 z 是否就绪。
    // 即使没有新行情也要重新检查过期，避免断流后继续按旧价差开仓。
    bool update(int64_t now) noexcept {
        if (mid_[0] <= 0 || mid_[1] <= 0) {
            ready_ = false;
            dirty_ = false;
            return false;
        }
        if (cfg_.max_stale_us > 0) {
            if (ts_[0] <= 0 || ts_[1] <= 0 || now - ts_[0] > cfg_.max_stale_us ||
                now - ts_[1] > cfg_.max_stale_us) {
                ready_ = false;
                dirty_ = false;
                return false;
            }
        }
        if (!dirty_) return ready_;
        if (cfg_.max_skew_us > 0) {
            const int64_t skew = ts_[0] >= ts_[1] ? ts_[0] - ts_[1] : ts_[1] - ts_[0];
            if (skew > cfg_.max_skew_us) {
                // 两腿不同步时不产生样本，等下一次对齐的报价再算价差。
                dirty_ = false;
                return ready_;
            }
        }
        dirty_ = false;
        spread_ = mid_[0] - mid_[1] * cfg_.ratio_ppm / 1000000;
        push(spread_);
        if (n_ < cfg_.window) {
            ready_ = false;
            return false;
        }
        mean_ = sum_ / n_;
        // 以均值为中心累加平方差；偏离上限防止极端跳变把 64 位累加器撑爆。
        int64_t var = 0;
        for (uint16_t i = 0; i < n_; ++i) {
            int64_t d = spread_ring_[i] - mean_;
            if (d > kMaxDeviation) d = kMaxDeviation;
            else if (d < -kMaxDeviation) d = -kMaxDeviation;
            var += d * d;
        }
        var /= n_;
        sd_ = static_cast<int64_t>(isqrt(static_cast<uint64_t>(var)));
        ready_ = true;
        if (sd_ <= 0) {
            z_ppm_ = 0;
            return true;
        }
        z_ppm_ = (spread_ - mean_) * 1000000 / sd_;
        return true;
    }

    // 交易线程推进状态机，输出两条腿的目标差额。调用方负责限速与报单。
    int step(int64_t now, PairReq* out, int cap) noexcept {
        if (!open_ || !out || cap < kMaxReq || paused_ || state_ == kPairStopped) return 0;
        (void)update(now);
        // 断流时两腿盘口都可能是旧值：只允许按持仓推进状态，不追单、不计超时，
        // 也不按旧价差做开平仓判断，等行情恢复后再继续。
        const bool live = fresh(now);
        switch (state_) {
            case kPairFlat:
                if (live && ready_ && (cfg_.max_rounds <= 0 || rounds_ < cfg_.max_rounds) &&
                    (last_exit_us_ == 0 || now - last_exit_us_ >= cfg_.cooldown_us)) {
                    if (z_ppm_ >= cfg_.entry_z_ppm) {
                        if (combo_ok(-1)) begin(-1, now);
                    } else if (z_ppm_ <= -cfg_.entry_z_ppm) {
                        if (combo_ok(1)) begin(1, now);
                    }
                }
                break;
            case kPairOpening:
                if (at_target()) {
                    state_ = kPairHolding;
                    hold_from_ = now;
                } else if (live && now - opened_us_ >= cfg_.leg_timeout_us) {
                    // 单腿裸露：超时后直接反向平掉已有的一腿，不继续追单。
                    const bool one_legged = (pos_[0] == 0) != (pos_[1] == 0);
                    if (one_legged) {
                        ++timeouts_;
                        begin_close(now);
                    } else {
                        pending_[0] = 0;
                        pending_[1] = 0;
                    }
                }
                break;
            case kPairHolding: {
                if (!live) break;
                bool close = false;
                // 入场同一时刻不判止损，给两腿成交和回报留一个节拍。
                if (now > hold_from_ && (z_ppm_ >= cfg_.stop_z_ppm || z_ppm_ <= -cfg_.stop_z_ppm)) {
                    ++stop_hits_;
                    close = true;
                } else if (z_ppm_ <= cfg_.exit_z_ppm && z_ppm_ >= -cfg_.exit_z_ppm) {
                    close = true;
                } else if (cfg_.max_hold_us > 0 && now - hold_from_ >= cfg_.max_hold_us) {
                    close = true;
                }
                if (close) begin_close(now);
                break;
            }
            case kPairClosing:
                if (pos_[0] == 0 && pos_[1] == 0) settle(now);
                else if (live && now - close_from_ >= cfg_.leg_timeout_us) {
                    ++timeouts_;
                    pending_[0] = 0;
                    pending_[1] = 0;
                }
                break;
            default:
                break;
        }
        if (!live) return 0;

        int n = 0;
        for (uint8_t leg = 0; leg < 2; ++leg) {
            // 顺序模式先完成主动腿，再发被动腿。
            if (cfg_.sequential && leg != cfg_.main_leg && pos_[cfg_.main_leg] != target_[cfg_.main_leg]) continue;
            int64_t delta = target_[leg] - pos_[leg] - pending_[leg];
            if (cfg_.max_leg_qty > 0) {
                if (delta > cfg_.max_leg_qty) delta = cfg_.max_leg_qty;
                else if (delta < -cfg_.max_leg_qty) delta = -cfg_.max_leg_qty;
            }
            if (delta == 0) continue;
            if (n >= cap) break;
            PairReq& r = out[n];
            r = PairReq{};
            r.leg = leg;
            r.name = leg == 0 ? cfg_.name_a : cfg_.name_b;
            r.qty = delta > 0 ? delta : -delta;
            r.side = delta > 0 ? 1 : 2;
            r.px = leg_px(leg, delta > 0);
            if (r.px <= 0) r.px = mid_[leg];
            if (r.px <= 0) continue;
            pending_[leg] += delta;
            pend_px_[leg] = r.px;
            pend_ts_[leg] = now;
            pend_live_[leg] = 1;
            ++n;
            ++orders_;
        }
        return n;
    }

    PairRow row() const noexcept {
        PairRow r{};
        r.state = state_;
        r.ready = ready_ ? 1 : 0;
        r.direction = dir_;
        r.spread = spread_;
        r.mean = mean_;
        r.sd = sd_;
        r.z_ppm = z_ppm_;
        r.pos_a = pos_[0];
        r.pos_b = pos_[1];
        r.target_a = target_[0];
        r.target_b = target_[1];
        r.rounds = rounds_;
        r.wins = wins_;
        r.losses = losses_;
        r.realized = realized_;
        r.signals = signals_;
        r.orders = orders_;
        r.rejects = rejects_;
        r.timeouts = timeouts_;
        r.stop_hits = stop_hits_;
        r.samples = n_;
        r.last_sig_us = last_sig_us_;
        r.opened_us = opened_us_;
        return r;
    }

private:
    // 主动腿吃对价；被动腿按 pending_ppm 从本方最优价向对方最优价让价。
    int64_t leg_px(uint8_t leg, bool buy) const noexcept {
        if (cfg_.pending_ppm > 0 && leg != cfg_.main_leg) {
            const int64_t spread = ask_[leg] - bid_[leg];
            if (bid_[leg] > 0 && spread > 0) {
                int64_t give = spread * cfg_.pending_ppm / 1000000;
                if (give > spread) give = spread;
                return buy ? bid_[leg] + give : ask_[leg] - give;
            }
        }
        return buy ? ask_[leg] : bid_[leg];
    }

    // 两腿行情都在过期窗口内才算新鲜。
    bool fresh(int64_t now) const noexcept {
        if (cfg_.max_stale_us <= 0) return true;
        if (ts_[0] <= 0 || ts_[1] <= 0) return false;
        return now - ts_[0] <= cfg_.max_stale_us && now - ts_[1] <= cfg_.max_stale_us;
    }

    void push(int64_t v) noexcept {
        if (static_cast<uint16_t>(n_) < cfg_.window) {
            spread_ring_[n_] = v;
            ++n_;
        } else {
            const int64_t old = spread_ring_[head_];
            sum_ -= old;
            spread_ring_[head_] = v;
            head_ = static_cast<uint16_t>((head_ + 1) % cfg_.window);
        }
        sum_ += v;
    }

    bool at_target() const noexcept {
        return pos_[0] == target_[0] && pos_[1] == target_[1];
    }

    void begin(int8_t dir, int64_t now) noexcept {
        dir_ = dir;
        target_[0] = dir * cfg_.qty_a;
        int64_t bq = cfg_.qty_a * cfg_.ratio_ppm / 1000000;
        if (bq <= 0) bq = 1;
        target_[1] = -dir * bq;
        state_ = kPairOpening;
        opened_us_ = now;
        last_sig_us_ = now;
        pending_[0] = 0;
        pending_[1] = 0;
        ++signals_;
    }

    void begin_close(int64_t now) noexcept {
        state_ = kPairClosing;
        target_[0] = 0;
        target_[1] = 0;
        close_from_ = now;
        pending_[0] = 0;
        pending_[1] = 0;
    }

    void apply_fill(uint8_t leg, int64_t signed_qty, int64_t px) noexcept {
        pos_[leg] += signed_qty;
        pending_[leg] -= signed_qty;
        if (pending_[leg] == 0) {
            pend_live_[leg] = 0;
            pend_ts_[leg] = 0;
        }
        cost_[leg] += px * signed_qty;
        const int64_t abs_qty = signed_qty > 0 ? signed_qty : -signed_qty;
        fees_ += px * abs_qty * cfg_.fee_ppm / 1000000;
    }

    void settle(int64_t now) noexcept {
        const int64_t pnl = -(cost_[0] + cost_[1]) - fees_;
        realized_ += pnl;
        if (pnl > 0) ++wins_;
        else if (pnl < 0) ++losses_;
        ++rounds_;
        cost_[0] = 0;
        cost_[1] = 0;
        fees_ = 0;
        state_ = kPairFlat;
        dir_ = 0;
        target_[0] = 0;
        target_[1] = 0;
        pending_[0] = 0;
        pending_[1] = 0;
        last_exit_us_ = now;
    }

    static uint64_t isqrt(uint64_t x) noexcept {
        uint64_t r = 0;
        uint64_t bit = 1ull << 62;
        while (bit > x) bit >>= 2;
        while (bit != 0) {
            if (x >= r + bit) {
                x -= r + bit;
                r = (r >> 1) + bit;
            } else {
                r >>= 1;
            }
            bit >>= 2;
        }
        return r;
    }

    PairCfg cfg_{};
    bool open_{false};
    bool paused_{false};
    uint8_t state_{kPairFlat};
    int8_t dir_{0};
    int64_t mid_[2]{};
    int64_t bid_[2]{};
    int64_t ask_[2]{};
    int64_t ts_[2]{};
    bool dirty_{false};
    static constexpr int64_t kMaxDeviation = 100000000;  // 价差偏离上限（价格 ×1e4 下约一万个价位）

    int64_t spread_ring_[kMaxWindow]{};
    uint16_t n_{0};
    uint16_t head_{0};
    int64_t sum_{0};
    int64_t spread_{0};
    int64_t mean_{0};
    int64_t sd_{0};
    int64_t z_ppm_{0};
    bool ready_{false};
    int64_t pos_[2]{};
    int64_t target_[2]{};
    int64_t pending_[2]{};
    int64_t pend_px_[2]{};
    int64_t pend_ts_[2]{};
    bool pend_live_[2]{};
    int64_t cost_[2]{};
    int64_t fees_{0};
    int64_t realized_{0};
    int64_t rounds_{0};
    int64_t wins_{0};
    int64_t losses_{0};
    int64_t signals_{0};
    int64_t orders_{0};
    int64_t rejects_{0};
    int64_t timeouts_{0};
    int64_t stop_hits_{0};
    int64_t last_sig_us_{0};
    int64_t last_exit_us_{0};
    int64_t opened_us_{0};
    int64_t hold_from_{0};
    int64_t close_from_{0};
};

}  // namespace pitlane
