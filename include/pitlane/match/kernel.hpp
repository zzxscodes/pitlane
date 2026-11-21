#pragma once

// 撮合内核：连续竞价与集合竞价共用一本价格-时间簿。
//
// * 连续竞价：价格优先、同价时间优先；支持 Day / Passive / Immediate / All(FOK)。
// * 集合竞价：Call 阶段只挂单，uncross() 以最大成交量为目标选出统一价，再按时间优先成交。
// * 队列位置：新挂单记录同价档位前方已有数量；成交回报按“先吃队首排队量，再成交”推进，
//   因此仿真与回测可以使用同一套排队模型。
// * 涨跌停、停牌、撤单、改单全部在内核里判定，结果由报单链路写回。
//
// 固定容量、无堆、无锁、无浮点。价格与数量都是整数。

#include <cstdint>

namespace pitlane {

enum MkPhase : uint8_t {
    kMkClosed = 0,
    kMkCall = 1,
    kMkContinuous = 2,
    kMkHalted = 3
};

enum MkWhy : uint8_t {
    kMkOk = 0,
    kMkWhyClosed = 1,
    kMkWhyHalted = 2,
    kMkWhyBand = 3,
    kMkWhyQty = 4,
    kMkWhySide = 5,
    kMkWhyDup = 6,
    kMkWhyFull = 7,
    kMkWhyMissing = 8,
    kMkWhyPrice = 9
};

inline const char* mk_why_name(uint8_t w) noexcept {
    switch (w) {
        case kMkWhyClosed: return "closed";
        case kMkWhyHalted: return "halted";
        case kMkWhyBand: return "band";
        case kMkWhyQty: return "qty";
        case kMkWhySide: return "side";
        case kMkWhyDup: return "dup";
        case kMkWhyFull: return "full";
        case kMkWhyMissing: return "missing";
        case kMkWhyPrice: return "price";
        default: return "ok";
    }
}

inline const char* mk_phase_name(uint8_t p) noexcept {
    switch (p) {
        case kMkCall: return "call";
        case kMkContinuous: return "continuous";
        case kMkHalted: return "halted";
        default: return "closed";
    }
}

struct MkIn {
    uint64_t tag{0};
    uint32_t acct{0};
    int64_t px{0};
    int64_t qty{0};
    int64_t ts_us{0};
    uint8_t side{0};  // 1 买，2 卖
    uint8_t style{1}; // 1 Day, 2 Passive, 3 Immediate, 4 All
};

struct MkFill {
    uint64_t taker{0};
    uint64_t maker{0};
    int64_t px{0};
    int64_t qty{0};
    uint8_t taker_side{0};
};

struct MkAck {
    uint64_t tag{0};
    uint8_t why{kMkOk};
    uint8_t phase{0};
    int64_t px{0};
    int64_t left{0};
    int64_t queue_ahead{0};
};

struct MkStats {
    uint64_t submits{0};
    uint64_t rests{0};
    uint64_t fills{0};
    uint64_t cancels{0};
    uint64_t amends{0};
    uint64_t rejects{0};
    uint64_t print_fills{0};
    uint64_t auction_fills{0};
    int64_t matched_qty{0};
};

struct MkLevelView {
    int64_t px{0};
    int64_t qty{0};
    int32_t orders{0};
    int32_t pad{0};
};

// 统一撮合内核。单线程写，调用方负责把同一时刻的报单/行情串行送入。
class MatchingKernel {
public:
    static constexpr int kOrders = 2048;
    static constexpr int kLevels = 256; // 每侧价格档上限
    static constexpr int kTagSlots = 8192;
    static constexpr int kDepthCap = 32;

    MatchingKernel() noexcept { reset(); }

    void reset() noexcept {
        for (int i = 0; i < kOrders; ++i) {
            nodes_[i] = Node{};
            nodes_[i].next = (i + 1 < kOrders) ? i + 1 : -1;
        }
        free_ = 0;
        live_ = 0;
        for (int i = 0; i < kLevels; ++i) {
            bids_[i] = Lvl{};
            asks_[i] = Lvl{};
        }
        nbid_ = 0;
        nask_ = 0;
        for (int i = 0; i < kTagSlots; ++i) {
            tag_key_[i] = 0;
            tag_node_[i] = -1;
            tag_state_[i] = 0;
        }
        phase_ = kMkClosed;
        prev_phase_ = kMkClosed;
        down_ = 0;
        up_ = 0;
        seq_ = 0;
        stats_ = MkStats{};
    }

    // 阶段切换。停牌会记住之前的阶段，resume() 恢复；集合竞价转连续由调用方决定。
    void set_phase(uint8_t p) noexcept {
        if (p == kMkHalted) {
            if (phase_ != kMkHalted) prev_phase_ = phase_;
            phase_ = kMkHalted;
            return;
        }
        phase_ = p;
        prev_phase_ = p;
    }
    void halt(bool on) noexcept {
        set_phase(on ? static_cast<uint8_t>(kMkHalted) : prev_phase_);
    }
    void resume() noexcept { phase_ = prev_phase_ == kMkHalted ? static_cast<uint8_t>(kMkContinuous) : prev_phase_; }
    uint8_t phase() const noexcept { return phase_; }

    // 涨跌停。任一为 0 表示不限制该方向。
    void set_limits(int64_t down, int64_t up) noexcept {
        down_ = down > 0 ? down : 0;
        up_ = up > 0 ? up : 0;
    }
    int64_t lower_limit() const noexcept { return down_; }
    int64_t upper_limit() const noexcept { return up_; }

    void set_queue_penalty(int64_t qty) noexcept { queue_penalty_ = qty > 0 ? qty : 0; }
    void set_self_trade_block(bool on) noexcept { block_self_ = on; }

    // 送单。fills 接收本次成交；ack 总会被填写。
    int submit(const MkIn& in, MkFill* fills, int cap, MkAck& ack) noexcept {
        ack = MkAck{};
        ack.tag = in.tag;
        ack.phase = phase_;
        ++stats_.submits;

        const uint8_t why = check(in);
        if (why != kMkOk) {
            ack.why = why;
            ++stats_.rejects;
            return 0;
        }
        if (tag_find(in.tag) >= 0) {
            ack.why = kMkWhyDup;
            ++stats_.rejects;
            return 0;
        }

        // 集合竞价只挂单，不撮合；连续竞价先吃掉对手盘。
        if (phase_ == kMkCall) {
            const int idx = rest(in, in.qty);
            if (idx < 0) {
                ack.why = kMkWhyFull;
                ++stats_.rejects;
                return 0;
            }
            ack.px = in.px;
            ack.left = in.qty;
            ack.queue_ahead = nodes_[idx].queue_ahead;
            return 0;
        }

        int n = 0;
        int64_t left = in.qty;
        if (in.style == static_cast<uint8_t>(4)) {
            const int64_t avail = available(in.side == 2 ? 1 : 2, in.px, in.acct);
            if (avail < in.qty) {
                ack.why = kMkWhyQty;
                ++stats_.rejects;
                return 0;
            }
        }
        if (in.style == static_cast<uint8_t>(2) && crosses(in)) {
            ack.why = kMkWhyPrice;
            ++stats_.rejects;
            return 0;
        }
        // 先吃对手盘；市价单（px<=0）不设价格约束。
        n = match(in, left, fills, cap);

        // Day 与未成交的 Passive 挂单；IOC/FOK 的剩余量直接撤销。
        if (left > 0 && in.px > 0 &&
            (in.style == static_cast<uint8_t>(1) || in.style == static_cast<uint8_t>(2))) {
            const int idx = rest(in, left);
            if (idx < 0) {
                ack.why = kMkWhyFull;
                ++stats_.rejects;
                ack.left = left;
                ack.px = in.px;
                return n;
            }
            ack.queue_ahead = nodes_[idx].queue_ahead;
        } else if (left > 0 && in.px <= 0) {
            // 市价剩余直接撤销。
        }
        ack.px = in.px;
        ack.left = left;
        return n;
    }

    // 撤单。成功时 leaves 返回剩余量。
    bool cancel(uint64_t tag, int64_t* leaves = nullptr) noexcept {
        const int idx = tag_find(tag);
        if (idx < 0) return false;
        if (leaves) *leaves = nodes_[idx].leaves;
        remove_node(idx);
        ++stats_.cancels;
        return true;
    }

    // 改单：撤旧挂新，队尾重新排队（交易所语义）。返回新增成交数。
    // 拒绝条件必须先于撤旧单检查：改单被拒时原单继续留在簿上。
    int amend(const MkIn& in, MkFill* fills, int cap, MkAck& ack) noexcept {
        ++stats_.amends;
        ack = MkAck{};
        ack.tag = in.tag;
        ack.phase = phase_;
        if (tag_find(in.tag) < 0) {
            ack.why = kMkWhyMissing;
            ++stats_.rejects;
            return 0;
        }
        const uint8_t why = check(in);
        if (why != kMkOk) {
            ack.why = why;
            ++stats_.rejects;
            return 0;
        }
        if (in.style == static_cast<uint8_t>(2) && crosses(in)) {
            ack.why = kMkWhyPrice;
            ++stats_.rejects;
            return 0;
        }
        if (in.style == static_cast<uint8_t>(4) && available(in.side == 2 ? 1 : 2, in.px, in.acct) < in.qty) {
            ack.why = kMkWhyQty;
            ++stats_.rejects;
            return 0;
        }
        int64_t old_left = 0;
        (void)cancel(in.tag, &old_left);
        return submit(in, fills, cap, ack);
    }

    // 逐笔成交驱动排队模型：主动方为 aggressor（1 买 2 卖）时，
    // 先消耗对手方向同价位的排队量，超出部分按时间优先成交。
    int on_print(int64_t px, int64_t qty, uint8_t aggressor, MkFill* fills, int cap) noexcept {
        if (px <= 0 || qty <= 0 || (aggressor != 1 && aggressor != 2)) return 0;
        const bool rest_is_bid = aggressor == 2;
        Lvl* levels = rest_is_bid ? bids_ : asks_;
        int& nl = rest_is_bid ? nbid_ : nask_;
        const int li = find_level(levels, nl, px, rest_is_bid);
        if (li < 0) return 0;

        int n = 0;
        int64_t left = qty;
        int idx = levels[li].head;
        while (idx >= 0 && left > 0) {
            Node& nd = nodes_[idx];
            const int nxt = nd.next;
            if (nd.queue_ahead > 0) {
                const int64_t eat = nd.queue_ahead < left ? nd.queue_ahead : left;
                nd.queue_ahead -= eat;
                left -= eat;
                if (left == 0) break;
            }
            const int64_t take = nd.leaves < left ? nd.leaves : left;
            if (take > 0) {
                emit(n, fills, cap, 0, nd.tag, px, take, aggressor == 1 ? 2 : 1);
                nd.leaves -= take;
                levels[li].qty -= take;
                left -= take;
                ++stats_.print_fills;
                stats_.matched_qty += take;
            }
            if (nd.leaves <= 0) {
                remove_node(idx);
                // remove_node 会移动档位数组，重新定位同价档。
                const int again = find_level(levels, nl, px, rest_is_bid);
                if (again < 0) break;
                idx = levels[again].head;
            } else {
                idx = nxt;
            }
        }
        return n;
    }

    // 集合竞价统一价：最大化成交量，平局时 tie_higher 取高价，否则取更接近 ref_px 的价。
    // 返回成交量；price_out 写入统一价（无成交写 0）。成交记录写入 fills。
    int64_t uncross(int64_t ref_px, int tie_higher, MkFill* fills, int cap, int64_t* price_out) noexcept {
        if (price_out) *price_out = 0;
        if (nbid_ == 0 || nask_ == 0) return 0;

        int64_t best_px = 0;
        int64_t best_vol = 0;
        int64_t best_dist = INT64_MAX;
        // 候选价取两侧所有档位价，档位数最多 2*kLevels。
        for (int side = 0; side < 2; ++side) {
            const Lvl* levels = side == 0 ? bids_ : asks_;
            const int nl = side == 0 ? nbid_ : nask_;
            for (int i = 0; i < nl; ++i) {
                const int64_t p = levels[i].px;
                const int64_t bvol = qty_at_or_better(bids_, nbid_, p, true);
                const int64_t svol = qty_at_or_better(asks_, nask_, p, false);
                const int64_t vol = bvol < svol ? bvol : svol;
                const int64_t dist = p > ref_px ? p - ref_px : ref_px - p;
                bool better = vol > best_vol;
                if (vol == best_vol && vol > 0) {
                    if (tie_higher) {
                        better = p > best_px;
                    } else {
                        better = dist < best_dist || (dist == best_dist && p > best_px);
                    }
                }
                if (better) {
                    best_vol = vol;
                    best_px = p;
                    best_dist = dist;
                }
            }
        }
        if (best_vol <= 0) return 0;

        int64_t left = best_vol;
        int n = 0;
        while (left > 0 && nbid_ > 0 && nask_ > 0) {
            Lvl& bl = bids_[0];
            Lvl& al = asks_[0];
            if (bl.px < best_px || al.px > best_px) break;
            if (bl.head < 0 || al.head < 0) break;
            const int bi = bl.head;
            const int ai = al.head;
            Node& bn = nodes_[bi];
            Node& an = nodes_[ai];
            if (block_self_ && bn.acct == an.acct) {
                // 自成交：跳过较晚到达的一侧，保持确定性与时间优先。
                if (bn.seq > an.seq) {
                    skip_head(bids_, nbid_);
                } else {
                    skip_head(asks_, nask_);
                }
                continue;
            }
            const int64_t take = bn.leaves < an.leaves ? bn.leaves : an.leaves;
            const int64_t q = take < left ? take : left;
            if (q <= 0) break;
            const uint64_t taker = bn.seq > an.seq ? bn.tag : an.tag;
            const uint64_t maker = bn.seq > an.seq ? an.tag : bn.tag;
            emit(n, fills, cap, taker, maker, best_px, q, static_cast<uint8_t>(bn.seq > an.seq ? 1 : 2));
            bn.leaves -= q;
            an.leaves -= q;
            bids_[0].qty -= q;
            asks_[0].qty -= q;
            left -= q;
            ++stats_.auction_fills;
            stats_.matched_qty += q;
            if (bn.leaves <= 0) remove_node(bi);
            if (an.leaves <= 0) remove_node(ai);
        }
        if (price_out) *price_out = best_px;
        return best_vol;
    }

    // 只读查询。
    bool best(int64_t* bid, int64_t* ask) const noexcept {
        if (bid) *bid = nbid_ > 0 ? bids_[0].px : 0;
        if (ask) *ask = nask_ > 0 ? asks_[0].px : 0;
        return nbid_ > 0 && nask_ > 0;
    }

    int64_t queue_ahead(uint64_t tag) const noexcept {
        const int idx = tag_find(tag);
        return idx >= 0 ? nodes_[idx].queue_ahead : -1;
    }

    int64_t leaves(uint64_t tag) const noexcept {
        const int idx = tag_find(tag);
        return idx >= 0 ? nodes_[idx].leaves : -1;
    }

    bool has(uint64_t tag) const noexcept { return tag_find(tag) >= 0; }
    int resting() const noexcept { return live_; }
    const MkStats& stats() const noexcept { return stats_; }

    int depth(uint8_t side, MkLevelView* out, int cap) const noexcept {
        if (!out || cap <= 0 || (side != 1 && side != 2)) return 0;
        const Lvl* levels = side == 1 ? bids_ : asks_;
        const int nl = side == 1 ? nbid_ : nask_;
        const int m = cap < nl ? cap : nl;
        for (int i = 0; i < m; ++i) {
            out[i].px = levels[i].px;
            out[i].qty = levels[i].qty;
            out[i].orders = levels[i].n;
            out[i].pad = 0;
        }
        return m;
    }

private:
    struct Lvl {
        int64_t px{0};
        int64_t qty{0};
        int32_t head{-1};
        int32_t tail{-1};
        int32_t n{0};
        int32_t pad{0};
    };
    struct Node {
        uint64_t tag{0};
        int64_t px{0};
        int64_t leaves{0};
        int64_t queue_ahead{0};
        int64_t ts_us{0};
        uint64_t seq{0};
        uint32_t acct{0};
        int32_t next{-1};
        int32_t prev{-1};
        uint8_t side{0};
        uint8_t style{0};
        uint8_t used{0};
        uint8_t pad{0};
    };

    uint8_t check(const MkIn& in) const noexcept {
        if (in.tag == 0 || in.qty <= 0) return kMkWhyQty;
        if (in.side != 1 && in.side != 2) return kMkWhySide;
        if (in.style < 1 || in.style > 5) return kMkWhyPrice;
        if (phase_ == kMkClosed) return kMkWhyClosed;
        if (phase_ == kMkHalted) return kMkWhyHalted;
        // 集合竞价必须有明确限价；市价单（含最优五档）只在连续竞价允许且必须 IOC。
        if (in.px <= 0) {
            if (phase_ != kMkContinuous || (in.style != 3 && in.style != 5)) return kMkWhyPrice;
            return kMkOk;
        }
        if (down_ > 0 && in.px < down_) return kMkWhyBand;
        if (up_ > 0 && in.px > up_) return kMkWhyBand;
        return kMkOk;
    }

    bool crosses(const MkIn& in) const noexcept {
        if (in.side == 1) return nask_ > 0 && in.px >= asks_[0].px;
        return nbid_ > 0 && in.px <= bids_[0].px;
    }

    // 对手方向、价格不劣于 in.px 的可见数量（不含自成交）。
    int64_t available(uint8_t side, int64_t px, uint32_t acct) const noexcept {
        const bool is_bid = side == 1;
        const Lvl* levels = is_bid ? bids_ : asks_;
        const int nl = is_bid ? nbid_ : nask_;
        int64_t sum = 0;
        for (int i = 0; i < nl; ++i) {
            if (px > 0 && (is_bid ? levels[i].px < px : levels[i].px > px)) break;
            for (int idx = levels[i].head; idx >= 0; idx = nodes_[idx].next) {
                if (block_self_ && nodes_[idx].acct == acct) continue;
                sum += nodes_[idx].leaves;
            }
        }
        return sum;
    }

    int match(const MkIn& in, int64_t& left, MkFill* fills, int cap) noexcept {
        const bool taker_bid = in.side == 1;
        Lvl* levels = taker_bid ? asks_ : bids_;
        int& nl = taker_bid ? nask_ : nbid_;
        int n = 0;
        // 最优五档（style 5）只允许穿透前五个价格档，剩余量由调用方撤销。
        int level_cap = (in.style == 5) ? 5 : (1 << 30);
        int levels_used = 0;
        while (left > 0 && nl > 0 && levels_used < level_cap) {
            Lvl& lv = levels[0];
            if (in.px > 0 && (taker_bid ? lv.px > in.px : lv.px < in.px)) break;
            int idx = lv.head;
            bool took = false;
            while (idx >= 0 && left > 0 && levels_used < level_cap) {
                Node& nd = nodes_[idx];
                const int nxt = nd.next;
                if (block_self_ && nd.acct == in.acct) {
                    idx = nxt;
                    continue;
                }
                const int64_t take = nd.leaves < left ? nd.leaves : left;
                emit(n, fills, cap, in.tag, nd.tag, nd.px, take, in.side);
                nd.leaves -= take;
                lv.qty -= take;
                left -= take;
                took = true;
                ++stats_.fills;
                stats_.matched_qty += take;
                if (nd.leaves <= 0) {
                    // 删除后档位数组可能前移，重新从队首继续。
                    remove_node(idx);
                    ++levels_used;
                    idx = levels[0].head;
                    nl = taker_bid ? nask_ : nbid_;
                    if (nl == 0) break;
                } else {
                    idx = nxt;
                }
            }
            if (!took) break;
            if (left <= 0 || levels_used >= level_cap) break;
            nl = taker_bid ? nask_ : nbid_;
            if (nl == 0) break;
        }
        return n;
    }

    int rest(const MkIn& in, int64_t qty) noexcept {
        if (free_ < 0) return -1;
        const bool is_bid = in.side == 1;
        Lvl* levels = is_bid ? bids_ : asks_;
        int& nl = is_bid ? nbid_ : nask_;
        int li = find_level(levels, nl, in.px, is_bid);
        int64_t ahead = 0;
        if (li < 0) {
            li = level_insert(levels, nl, in.px, is_bid);
            if (li < 0) return -1;
        } else {
            ahead = levels[li].qty + queue_penalty_;
        }

        const int idx = free_;
        Node& nd = nodes_[idx];
        free_ = nd.next;
        nd = Node{};
        nd.tag = in.tag;
        nd.px = in.px;
        nd.leaves = qty;
        nd.queue_ahead = ahead;
        nd.ts_us = in.ts_us;
        nd.seq = ++seq_;
        nd.acct = in.acct;
        nd.side = in.side;
        nd.style = in.style;
        nd.used = 1;
        nd.next = -1;
        nd.prev = levels[li].tail;
        if (levels[li].tail >= 0) nodes_[levels[li].tail].next = idx;
        else levels[li].head = idx;
        levels[li].tail = idx;
        levels[li].qty += qty;
        ++levels[li].n;
        ++live_;
        tag_put(in.tag, idx);
        ++stats_.rests;
        return idx;
    }

    void remove_node(int idx) noexcept {
        if (idx < 0 || !nodes_[idx].used) return;
        Node& nd = nodes_[idx];
        const bool is_bid = nd.side == 1;
        Lvl* levels = is_bid ? bids_ : asks_;
        int& nl = is_bid ? nbid_ : nask_;
        const int li = find_level(levels, nl, nd.px, is_bid);
        if (li >= 0) {
            Lvl& lv = levels[li];
            if (nd.prev >= 0) nodes_[nd.prev].next = nd.next;
            else lv.head = nd.next;
            if (nd.next >= 0) nodes_[nd.next].prev = nd.prev;
            else lv.tail = nd.prev;
            lv.qty -= nd.leaves;
            if (lv.qty < 0) lv.qty = 0;
            --lv.n;
            if (lv.n <= 0) level_erase(levels, nl, li);
        }
        tag_del(nd.tag);
        nd.used = 0;
        nd.next = free_;
        free_ = idx;
        --live_;
    }

    void skip_head(Lvl* levels, int& nl) noexcept {
        if (nl <= 0) return;
        const int idx = levels[0].head;
        if (idx < 0) return;
        remove_node(idx);
    }

    static bool better(const Lvl& lv, int64_t px, bool is_bid) noexcept {
        return is_bid ? lv.px > px : lv.px < px;
    }

    static int find_level(const Lvl* levels, int nl, int64_t px, bool is_bid) noexcept {
        for (int i = 0; i < nl; ++i) {
            if (levels[i].px == px) return i;
            if (!better(levels[i], px, is_bid)) return -1;
        }
        return -1;
    }

    static int level_insert(Lvl* levels, int& nl, int64_t px, bool is_bid) noexcept {
        if (nl >= kLevels) return -1;
        int at = 0;
        while (at < nl && better(levels[at], px, is_bid)) ++at;
        for (int j = nl; j > at; --j) levels[j] = levels[j - 1];
        levels[at] = Lvl{};
        levels[at].px = px;
        ++nl;
        return at;
    }

    static void level_erase(Lvl* levels, int& nl, int at) noexcept {
        for (int j = at; j < nl - 1; ++j) levels[j] = levels[j + 1];
        --nl;
        if (nl >= 0) levels[nl] = Lvl{};
    }

    static int64_t qty_at_or_better(const Lvl* levels, int nl, int64_t px, bool is_bid_better) noexcept {
        int64_t sum = 0;
        for (int i = 0; i < nl; ++i) {
            if (is_bid_better ? levels[i].px < px : levels[i].px > px) break;
            sum += levels[i].qty;
        }
        return sum;
    }

    static void emit(int& n, MkFill* fills, int cap, uint64_t taker, uint64_t maker, int64_t px, int64_t qty,
                     uint8_t taker_side) noexcept {
        if (!fills || n >= cap) return;
        MkFill& f = fills[n++];
        f.taker = taker;
        f.maker = maker;
        f.px = px;
        f.qty = qty;
        f.taker_side = taker_side;
    }

    static uint32_t tag_hash(uint64_t x) noexcept {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33;
        return static_cast<uint32_t>(x);
    }

    int tag_find(uint64_t tag) const noexcept {
        if (tag == 0) return -1;
        uint32_t h = tag_hash(tag) & (kTagSlots - 1u);
        for (int probe = 0; probe < kTagSlots; ++probe) {
            if (tag_state_[h] == 0) return -1;
            if (tag_state_[h] == 1 && tag_key_[h] == tag) return tag_node_[h];
            h = (h + 1) & (kTagSlots - 1u);
        }
        return -1;
    }

    void tag_put(uint64_t tag, int node) noexcept {
        uint32_t h = tag_hash(tag) & (kTagSlots - 1u);
        uint32_t tomb = UINT32_MAX;
        for (int probe = 0; probe < kTagSlots; ++probe) {
            if (tag_state_[h] == 0) {
                if (tomb != UINT32_MAX) h = tomb;
                tag_key_[h] = tag;
                tag_node_[h] = node;
                tag_state_[h] = 1;
                return;
            }
            if (tag_state_[h] == 1 && tag_key_[h] == tag) {
                tag_node_[h] = node;
                return;
            }
            if (tag_state_[h] == 2 && tomb == UINT32_MAX) tomb = h;
            h = (h + 1) & (kTagSlots - 1u);
        }
    }

    void tag_del(uint64_t tag) noexcept {
        uint32_t h = tag_hash(tag) & (kTagSlots - 1u);
        for (int probe = 0; probe < kTagSlots; ++probe) {
            if (tag_state_[h] == 0) return;
            if (tag_state_[h] == 1 && tag_key_[h] == tag) {
                tag_state_[h] = 2;
                tag_node_[h] = -1;
                return;
            }
            h = (h + 1) & (kTagSlots - 1u);
        }
    }

    Node nodes_[kOrders]{};
    Lvl bids_[kLevels]{};
    Lvl asks_[kLevels]{};
    int nbid_{0};
    int nask_{0};
    int free_{-1};
    int live_{0};
    uint64_t seq_{0};
    uint8_t phase_{kMkClosed};
    uint8_t prev_phase_{kMkClosed};
    int64_t down_{0};
    int64_t up_{0};
    int64_t queue_penalty_{0};
    bool block_self_{true};
    uint64_t tag_key_[kTagSlots]{};
    int32_t tag_node_[kTagSlots]{};
    uint8_t tag_state_[kTagSlots]{};
    MkStats stats_{};
};

} // namespace pitlane
