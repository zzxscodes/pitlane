#pragma once

// Adaptive parent-order execution. A parent quantity is worked against a
// schedule: a straight line for TWAP, an optional volume curve for VWAP, or an
// untimed pass-through. The controller only emits plain child commands; the
// caller mints ids through `id_base`, books the order, and routes it.
//
// The decision loop is a fixed state machine:
//
//   behind the schedule by `catchup_ppm`  -> cross the spread, Immediate
//   at or ahead of the schedule           -> join the touch, Day
//   a working child is older than reorder -> cancel it once, then repost
//   past the end plus `timeout_us`        -> catch up aggressively
//
// Passive and aggressive prices come from `quoteprice.hpp`; size is capped by
// the displayed quantity at `participation_ppm`, by `max_clip`, and by whole
// lots. A locked market with `one_touch` sends one child and then waits.
//
// All state is a compile-time array of integers. Nothing allocates, nothing
// locks, and a rejected transition leaves the object unchanged.

#include "pitlane/desk/types.hpp"
#include "pitlane/order/quoteprice.hpp"
#include "pitlane/util/numeric.hpp"

#include <cstdint>

namespace pitlane {

enum class ExecStyle : uint8_t {
    Pass = 1,  // send at the touch with no schedule
    Twap = 2,  // linear cumulative target
    Vwap = 3,  // cumulative target follows the weight curve
};

enum class ExecState : uint8_t {
    Idle = 0,
    Passive = 1,
    Aggressive = 2,
    Waiting = 3,
    Paused = 4,
    Done = 5,
    Stopped = 6,
    Failed = 7,
};

inline const char* exec_state_name(uint8_t s) noexcept {
    switch (static_cast<ExecState>(s)) {
        case ExecState::Passive: return "passive";
        case ExecState::Aggressive: return "aggressive";
        case ExecState::Waiting: return "waiting";
        case ExecState::Paused: return "paused";
        case ExecState::Done: return "done";
        case ExecState::Stopped: return "stopped";
        case ExecState::Failed: return "failed";
        default: return "idle";
    }
}

enum : uint8_t { kExecPlace = 1, kExecCancel = 2 };

struct ExecCfg {
    uint32_t name{0};
    uint8_t side{1};              // Side
    uint8_t style{2};             // ExecStyle
    int64_t total{0};
    int64_t t0_us{0};
    int64_t end_us{0};
    int64_t lot{1};
    int64_t tick{1};
    int64_t max_clip{0};          // 0 = no per-child cap
    int64_t min_clip{0};          // 0 = one lot
    int64_t participation_ppm{0}; // 0 = no participation cap
    int64_t catchup_ppm{200000};  // behind by this share of the parent -> aggressive
    int64_t reorder_us{300000};   // cancel a child that is still working after this
    int64_t timeout_us{0};        // extra time after end before the hard catch-up
    int64_t stale_us{3000000};    // a quote older than this parks the schedule
    int32_t passive_steps{0};     // join the touch, stepped inside the spread
    int32_t aggressive_steps{0};  // cross the touch by this many ticks
    uint8_t one_touch{0};         // at a locked market send once, then wait
    uint8_t pad[3]{};
    int64_t up_limit{0};          // 0 = unknown
    int64_t down_limit{0};        // 0 = unknown
    uint8_t allow_at_limit{0};    // 0 = stop once the market is pinned at the band
    uint8_t pad2[7]{};
    uint64_t id_base{1};          // child ids are id_base + place sequence
    const int64_t* curve{nullptr};  // Vwap weights, one per equal time bucket
    int curve_n{0};
};

// One emitted decision. `id` is assigned by the controller.
struct ExecCmd {
    uint8_t kind{0};   // kExecPlace / kExecCancel
    uint8_t side{0};
    uint8_t style{0};  // Style
    uint8_t reason{0}; // 0 passive, 1 aggressive, 2 late, 3 locked
    uint64_t id{0};
    int64_t px{0};
    int64_t qty{0};
};

struct ExecRow {
    uint8_t state{0};
    uint8_t paused{0};
    uint8_t working{0};
    uint8_t one_touch_used{0};
    int64_t total{0};
    int64_t done{0};
    int64_t working_qty{0};
    int64_t target{0};
    int64_t remaining{0};
    int64_t avg_px{0};
    uint64_t sends{0};
    uint64_t fills{0};
    uint64_t cancels{0};
    uint64_t rejects{0};
};

class ExecAlgo {
public:
    static constexpr int kCurveMax = 64;

    ExecState state() const noexcept { return static_cast<ExecState>(st_); }
    bool active() const noexcept { return st_ != 0 && st_ != 5 && st_ != 6 && st_ != 7; }
    bool working() const noexcept { return working_ != 0; }
    int64_t done() const noexcept { return done_; }
    int64_t remaining() const noexcept { return cfg_.total > done_ ? cfg_.total - done_ : 0; }

    bool open(const ExecCfg& cfg, int64_t now_us) noexcept {
        cfg_ = cfg;
        st_ = 0;
        paused_ = 0;
        working_ = 0;
        cancel_sent_ = 0;
        one_touch_used_ = 0;
        limit_hit_ = 0;
        done_ = 0;
        filled_notional_ = 0;
        working_id_ = 0;
        working_px_ = 0;
        working_qty_ = 0;
        work_us_ = 0;
        quote_ts_ = 0;
        has_quote_ = 0;
        last_target_ = 0;
        curve_n_ = 0;
        weight_sum_ = 0;
        sends_ = fills_ = cancels_ = rejects_ = 0;
        seq_ = 0;
        quote_ = QuoteTop{};
        if (cfg_.name == 0 || !known_side(cfg_.side) || cfg_.total <= 0 || cfg_.lot <= 0 || cfg_.tick <= 0) {
            st_ = static_cast<uint8_t>(ExecState::Failed);
            return false;
        }
        if (cfg_.end_us <= cfg_.t0_us) {
            st_ = static_cast<uint8_t>(ExecState::Failed);
            return false;
        }
        if (cfg_.style != static_cast<uint8_t>(ExecStyle::Pass) && cfg_.style != static_cast<uint8_t>(ExecStyle::Twap) &&
            cfg_.style != static_cast<uint8_t>(ExecStyle::Vwap)) {
            st_ = static_cast<uint8_t>(ExecState::Failed);
            return false;
        }
        if (cfg_.min_clip < 0 || cfg_.max_clip < 0 || cfg_.participation_ppm < 0 || cfg_.catchup_ppm < 0 ||
            cfg_.reorder_us < 0 || cfg_.timeout_us < 0 || cfg_.stale_us < 0 || cfg_.up_limit < 0 ||
            cfg_.down_limit < 0) {
            st_ = static_cast<uint8_t>(ExecState::Failed);
            return false;
        }
        if (cfg_.up_limit > 0 && cfg_.down_limit > 0 && cfg_.up_limit <= cfg_.down_limit) {
            st_ = static_cast<uint8_t>(ExecState::Failed);
            return false;
        }
        if (cfg_.style == static_cast<uint8_t>(ExecStyle::Vwap)) {
            if (!cfg_.curve || cfg_.curve_n <= 0 || cfg_.curve_n > kCurveMax) {
                st_ = static_cast<uint8_t>(ExecState::Failed);
                return false;
            }
            for (int i = 0; i < cfg_.curve_n; ++i) {
                const int64_t w = cfg_.curve[i];
                if (w < 0 || weight_sum_ > INT64_MAX - w) {
                    st_ = static_cast<uint8_t>(ExecState::Failed);
                    return false;
                }
                curve_[i] = w;
                weight_sum_ += w;
            }
            if (weight_sum_ <= 0) {
                st_ = static_cast<uint8_t>(ExecState::Failed);
                return false;
            }
            curve_n_ = cfg_.curve_n;
        }
        cfg_.curve = nullptr;
        cfg_.curve_n = curve_n_;
        if (now_us >= cfg_.end_us) {
            // Already past the horizon: the whole parent is late.
            st_ = static_cast<uint8_t>(ExecState::Aggressive);
        } else {
            st_ = static_cast<uint8_t>(ExecState::Waiting);
        }
        return true;
    }

    void on_quote(const QuoteTop& q, int64_t now_us) noexcept {
        quote_ = q;
        quote_ts_ = now_us;
        has_quote_ = 1;
        if (!active() || cfg_.allow_at_limit || limit_hit_) return;
        if (cfg_.side == static_cast<uint8_t>(Side::Buy)) {
            if (cfg_.down_limit > 0 && q.ask[0] > 0 && q.ask[0] <= cfg_.down_limit) limit_hit_ = 1;
        } else {
            if (cfg_.up_limit > 0 && q.bid[0] > 0 && q.bid[0] >= cfg_.up_limit) limit_hit_ = 1;
        }
    }

    bool pause() noexcept {
        if (!active() || paused_) return false;
        paused_ = 1;
        st_ = static_cast<uint8_t>(ExecState::Paused);
        return true;
    }

    bool resume() noexcept {
        if (!active() || !paused_) return false;
        paused_ = 0;
        st_ = static_cast<uint8_t>(ExecState::Waiting);
        return true;
    }

    bool stop() noexcept {
        if (!active()) return false;
        st_ = static_cast<uint8_t>(ExecState::Stopped);
        paused_ = 0;
        return true;
    }

    void on_ack(uint64_t id) noexcept {
        if (working_ && id == working_id_) cancel_sent_ = 0;
    }

    // `px` is the fill price in the caller's fixed point.
    void on_fill(int64_t qty, int64_t px) noexcept {
        if (qty <= 0) return;
        int64_t take = qty;
        if (working_qty_ > 0 && take > working_qty_) take = working_qty_;
        working_qty_ -= take;
        if (working_qty_ <= 0) {
            working_qty_ = 0;
            working_ = 0;
            cancel_sent_ = 0;
        }
        done_ += take;
        ++fills_;
        if (px > 0) {
            if (take > INT64_MAX / px) {
                filled_notional_ = INT64_MAX;
            } else if (filled_notional_ <= INT64_MAX - take * px) {
                filled_notional_ += take * px;
            } else {
                filled_notional_ = INT64_MAX;
            }
        }
        if (done_ >= cfg_.total) {
            done_ = cfg_.total;
            st_ = static_cast<uint8_t>(ExecState::Done);
            working_ = 0;
            working_qty_ = 0;
            cancel_sent_ = 0;
        }
    }

    void on_cancel() noexcept {
        working_ = 0;
        working_qty_ = 0;
        working_id_ = 0;
        working_px_ = 0;
        cancel_sent_ = 0;
    }

    void on_reject() noexcept {
        ++rejects_;
        working_ = 0;
        working_qty_ = 0;
        working_id_ = 0;
        working_px_ = 0;
        cancel_sent_ = 0;
        if (active()) st_ = static_cast<uint8_t>(ExecState::Waiting);
    }

    int64_t target_at(int64_t now_us) const noexcept {
        if (cfg_.total <= 0) return 0;
        if (now_us <= cfg_.t0_us) return 0;
        if (now_us >= cfg_.end_us) return cfg_.total;
        const int64_t elapsed = now_us - cfg_.t0_us;
        const int64_t horizon = cfg_.end_us - cfg_.t0_us;
        if (cfg_.style == static_cast<uint8_t>(ExecStyle::Pass)) return cfg_.total;
        if (curve_n_ <= 0) {
            const int64_t t = mul_div(cfg_.total, elapsed, horizon);
            return t < 0 ? cfg_.total : t;
        }
        int bucket = static_cast<int>(elapsed * curve_n_ / horizon);
        if (bucket < 0) bucket = 0;
        if (bucket >= curve_n_) bucket = curve_n_ - 1;
        const int64_t edge = horizon * (bucket + 1) / curve_n_;
        int64_t prefix = 0;
        for (int i = 0; i < bucket; ++i) prefix += curve_[i];
        const int64_t span = edge - elapsed;  // time left in this bucket
        if (span <= 0) return cfg_.total;
        // Weight consumed so far: the prefix plus the fraction of this bucket
        // that has already elapsed. ppm is in millionths of the full curve.
        const int64_t bucket_time = edge - horizon * bucket / curve_n_;
        const int64_t elapsed_in = bucket_time - span;
        const int64_t part = prefix * 1000000;
        const int64_t frac =
            bucket_time > 0 ? mul_div(curve_[bucket] * 1000000, elapsed_in, bucket_time) : curve_[bucket] * 1000000;
        const int64_t ppm = (part + frac) / weight_sum_;
        const int64_t t = mul_div(cfg_.total, ppm, 1000000);
        return t < 0 ? 0 : t;
    }

    // Emits at most one child command. Returns 1 when `out[0]` is set.
    int poll(int64_t now_us, ExecCmd* out, int cap) noexcept {
        if (!out || cap <= 0 || paused_ || !active()) return 0;
        if (done_ >= cfg_.total) {
            st_ = static_cast<uint8_t>(ExecState::Done);
            return 0;
        }
        if (limit_hit_) {
            st_ = static_cast<uint8_t>(ExecState::Stopped);
            return 0;
        }
        if (!has_quote_ || (cfg_.stale_us > 0 && now_us - quote_ts_ > cfg_.stale_us)) {
            st_ = static_cast<uint8_t>(ExecState::Waiting);
            return 0;
        }
        if (cancel_sent_) return 0;  // wait for the cancel ack

        const int64_t target = target_at(now_us);
        last_target_ = target;
        const int64_t behind = target - done_ - working_qty_;
        const bool late = (cfg_.timeout_us >= 0 && now_us > cfg_.end_us + cfg_.timeout_us) || target >= cfg_.total;

        if (working_) {
            if (now_us - work_us_ < cfg_.reorder_us) return 0;
            const Decision d = decide(behind, late);
            if (d.ok && d.px == working_px_) return 0;
            ExecCmd& c = out[0];
            c = ExecCmd{};
            c.kind = kExecCancel;
            c.side = cfg_.side;
            c.id = working_id_;
            c.px = working_px_;
            c.qty = working_qty_;
            cancel_sent_ = 1;
            ++cancels_;
            return 1;
        }

        Decision d = decide(behind, late);
        if (!d.ok) {
            st_ = static_cast<uint8_t>(ExecState::Waiting);
            return 0;
        }
        int64_t qty = d.qty;
        if (qty <= 0) {
            st_ = static_cast<uint8_t>(ExecState::Waiting);
            return 0;
        }
        if (qty > qty_left()) qty = qty_left();
        const uint64_t id = cfg_.id_base + seq_;
        if (id == 0) {
            st_ = static_cast<uint8_t>(ExecState::Failed);
            return 0;
        }
        ExecCmd& c = out[0];
        c = ExecCmd{};
        c.kind = kExecPlace;
        c.side = cfg_.side;
        c.style = d.style;
        c.reason = d.reason;
        c.id = id;
        c.px = d.px;
        c.qty = qty;
        ++seq_;
        ++sends_;
        working_ = 1;
        working_id_ = id;
        working_px_ = d.px;
        working_qty_ = qty;
        work_us_ = now_us;
        if (d.reason == 3) one_touch_used_ = 1;
        st_ = static_cast<uint8_t>(d.reason == 1 || d.reason == 2 ? ExecState::Aggressive : ExecState::Passive);
        return 1;
    }

    ExecRow row() const noexcept {
        ExecRow r{};
        r.state = st_;
        r.paused = paused_;
        r.working = working_;
        r.one_touch_used = one_touch_used_;
        r.total = cfg_.total;
        r.done = done_;
        r.working_qty = working_qty_;
        r.target = last_target_;
        r.remaining = remaining();
        r.avg_px = done_ > 0 ? filled_notional_ / done_ : 0;
        r.sends = sends_;
        r.fills = fills_;
        r.cancels = cancels_;
        r.rejects = rejects_;
        return r;
    }

private:
    struct Decision {
        bool ok{false};
        int64_t px{0};
        int64_t qty{0};
        uint8_t style{1};
        uint8_t reason{0};
    };

    static int64_t mul_div(int64_t a, int64_t b, int64_t c) noexcept {
        if (a < 0 || b < 0 || c <= 0) return -1;
        const Wide w = mul_wide(static_cast<uint64_t>(a), static_cast<uint64_t>(b));
        uint64_t q = 0;
        if (!div_wide(w, static_cast<uint64_t>(c), q) || q > static_cast<uint64_t>(INT64_MAX)) return -1;
        return static_cast<int64_t>(q);
    }

    int64_t qty_left() const noexcept { return cfg_.total > done_ ? cfg_.total - done_ : 0; }

    Decision decide(int64_t behind, bool late) const noexcept {
        Decision d{};
        if (behind <= 0) return d;
        const bool buy = cfg_.side == static_cast<uint8_t>(Side::Buy);
        const bool locked = quote_.bid[0] > 0 && quote_.bid[0] == quote_.ask[0];
        if (locked && cfg_.one_touch) {
            if (one_touch_used_) return d;
            if (quote_.ask[0] <= 0) return d;
            int64_t qty = behind;
            if (cfg_.max_clip > 0 && qty > cfg_.max_clip) qty = cfg_.max_clip;
            qty = (qty / cfg_.lot) * cfg_.lot;
            if (qty <= 0) return d;
            d.ok = true;
            d.px = quote_.ask[0];
            d.qty = qty;
            d.style = static_cast<uint8_t>(Style::Immediate);
            d.reason = 3;
            return d;
        }

        int64_t catchup = cfg_.min_clip > 0 ? cfg_.min_clip : cfg_.lot;
        const int64_t share = mul_div(cfg_.total, cfg_.catchup_ppm, 1000000);
        if (share > catchup) catchup = share;
        const bool urgent = late || behind >= catchup;

        int64_t px = urgent ? chasing_px(buy, 0, quote_, cfg_.tick, cfg_.aggressive_steps)
                            : post_px(buy, quote_.bid[0], quote_.ask[0], cfg_.tick, cfg_.passive_steps);
        if (px <= 0) px = pre_px(buy, 0, quote_, true, false);
        if (px <= 0) return d;

        int64_t qty = behind;
        if (qty > qty_left()) qty = qty_left();
        if (cfg_.max_clip > 0 && qty > cfg_.max_clip) qty = cfg_.max_clip;
        if (cfg_.participation_ppm > 0) {
            const int64_t shown = buy ? (urgent ? quote_.ask_qty : quote_.bid_qty)
                                      : (urgent ? quote_.bid_qty : quote_.ask_qty);
            if (shown > 0) {
                const int64_t cap = mul_div(shown, cfg_.participation_ppm, 1000000);
                if (cap <= 0) return d;
                if (qty > cap) qty = cap;
            }
        }
        qty = (qty / cfg_.lot) * cfg_.lot;
        if (qty <= 0) return d;
        if (!urgent && cfg_.min_clip > 0 && qty < cfg_.min_clip && behind >= cfg_.min_clip) return d;
        d.ok = true;
        d.px = px;
        d.qty = qty;
        d.style = urgent ? static_cast<uint8_t>(Style::Immediate) : static_cast<uint8_t>(Style::Day);
        d.reason = late ? 2 : (urgent ? 1 : 0);
        return d;
    }

    ExecCfg cfg_{};
    QuoteTop quote_{};
    int64_t curve_[kCurveMax]{};
    int64_t weight_sum_{0};
    int curve_n_{0};
    uint8_t st_{0};
    uint8_t paused_{0};
    uint8_t working_{0};
    uint8_t cancel_sent_{0};
    uint8_t one_touch_used_{0};
    uint8_t limit_hit_{0};
    int64_t done_{0};
    int64_t filled_notional_{0};
    uint64_t working_id_{0};
    int64_t working_px_{0};
    int64_t working_qty_{0};
    int64_t work_us_{0};
    int64_t quote_ts_{0};
    int64_t last_target_{0};
    uint8_t has_quote_{0};
    uint64_t seq_{0};
    uint64_t sends_{0};
    uint64_t fills_{0};
    uint64_t cancels_{0};
    uint64_t rejects_{0};
};

}  // namespace pitlane
