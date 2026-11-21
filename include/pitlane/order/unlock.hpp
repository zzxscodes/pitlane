#pragma once

#include <cstdint>

namespace pitlane {

// Releases a locked position: one long leg and one short leg on the same name.
// The plan reduces both sides in step. The paired goal is the smaller locked
// quantity rounded down to whole lots. Each step emits only the leg that is
// still short of the goal, so a partial fill on one side is completed on the
// next step instead of reopening the lock. Counters belong to the caller, the
// plan never allocates, and every quantity is an integer.

enum : uint8_t { kUnlockLongLeg = 0, kUnlockShortLeg = 1 };

struct UnlockCfg {
    int64_t lot{100};     // whole-lot step, must be positive
    int64_t clip{0};      // largest quantity per leg per step, 0 = no cap
    int64_t min_clip{0};  // skip a clip below this, 0 = only require one lot
};

// One leg to send. Leg 0 sells the long side, leg 1 buys back the short side.
struct UnlockReq {
    uint8_t leg{0};
    uint8_t side{0};  // 1 buy, 2 sell
    int64_t qty{0};
};

struct UnlockRow {
    int64_t locked_long{0};
    int64_t locked_short{0};
    int64_t goal{0};
    int64_t done_long{0};
    int64_t done_short{0};
    int64_t pend_long{0};
    int64_t pend_short{0};
    int64_t residual_long{0};   // locked long left once the goal is reached
    int64_t residual_short{0};  // locked short left once the goal is reached
    int64_t steps{0};
    int64_t fills{0};
    int64_t cancels{0};
    int64_t rejects{0};
    uint8_t done{0};
};

class UnlockPlan {
public:
    // The two quantities are what the account may close today, not the full
    // holding. A negative quantity or a zero lot is refused.
    bool open(const UnlockCfg& c, int64_t long_qty, int64_t short_qty) noexcept {
        if (c.lot <= 0 || c.clip < 0 || c.min_clip < 0) return false;
        if (c.clip > 0 && c.min_clip > c.clip) return false;
        if (long_qty < 0 || short_qty < 0) return false;
        cfg_ = c;
        init_[0] = long_qty;
        init_[1] = short_qty;
        const int64_t pair = long_qty < short_qty ? long_qty : short_qty;
        goal_ = (pair / c.lot) * c.lot;
        for (int i = 0; i < 2; ++i) {
            done_[i] = 0;
            pend_[i] = 0;
        }
        steps_ = 0;
        fills_ = 0;
        cancels_ = 0;
        rejects_ = 0;
        open_ = true;
        return true;
    }

    bool is_open() const noexcept { return open_; }
    int64_t goal() const noexcept { return goal_; }
    bool done() const noexcept { return open_ && done_[0] == goal_ && done_[1] == goal_; }

    // Emits at most one child per leg. A leg already at the goal is skipped, so
    // a fill that lands on one side only leaves the other side to be completed
    // by the next call. The pending quantity set here is released by
    // on_fill, on_cancel, or on_reject.
    int step(UnlockReq* out, int cap) noexcept {
        if (!open_ || !out || cap <= 0 || goal_ <= 0) return 0;
        int n = 0;
        for (uint8_t leg = 0; leg < 2 && n < cap; ++leg) {
            int64_t gap = goal_ - done_[leg] - pend_[leg];
            if (gap <= 0) continue;
            if (cfg_.clip > 0 && gap > cfg_.clip) gap = cfg_.clip;
            gap = (gap / cfg_.lot) * cfg_.lot;
            if (gap <= 0) continue;
            if (cfg_.min_clip > 0 && gap < cfg_.min_clip) continue;
            UnlockReq& r = out[n];
            r = UnlockReq{};
            r.leg = leg;
            r.side = leg == kUnlockLongLeg ? 2 : 1;
            r.qty = gap;
            pend_[leg] += gap;
            ++n;
        }
        if (n > 0) ++steps_;
        return n;
    }

    // A fill above what the goal still needs is clamped; the excess is ignored
    // because the locked quantity is fixed when the plan opens.
    bool on_fill(uint8_t leg, int64_t qty) noexcept {
        if (!open_ || leg > 1 || qty <= 0) return false;
        const int64_t room = goal_ - done_[leg];
        if (room <= 0) return false;
        const int64_t take = qty < room ? qty : room;
        done_[leg] += take;
        pend_[leg] -= take;
        if (pend_[leg] < 0) pend_[leg] = 0;
        ++fills_;
        return true;
    }

    // A cancelled or rejected child returns its quantity to the plan, which
    // then re-emits it. A quantity larger than the pending amount is clamped.
    bool on_cancel(uint8_t leg, int64_t qty) noexcept {
        if (!open_ || leg > 1 || qty <= 0) return false;
        if (pend_[leg] <= 0) return false;
        pend_[leg] -= qty;
        if (pend_[leg] < 0) pend_[leg] = 0;
        ++cancels_;
        return true;
    }

    bool on_reject(uint8_t leg, int64_t qty) noexcept {
        if (!open_ || leg > 1 || qty <= 0) return false;
        if (pend_[leg] <= 0) return false;
        pend_[leg] -= qty;
        if (pend_[leg] < 0) pend_[leg] = 0;
        ++rejects_;
        return true;
    }

    UnlockRow row() const noexcept {
        UnlockRow r{};
        r.locked_long = init_[0];
        r.locked_short = init_[1];
        r.goal = goal_;
        r.done_long = done_[0];
        r.done_short = done_[1];
        r.pend_long = pend_[0];
        r.pend_short = pend_[1];
        r.residual_long = init_[0] - done_[0];
        r.residual_short = init_[1] - done_[1];
        if (r.residual_long < 0) r.residual_long = 0;
        if (r.residual_short < 0) r.residual_short = 0;
        r.steps = steps_;
        r.fills = fills_;
        r.cancels = cancels_;
        r.rejects = rejects_;
        r.done = done() ? 1 : 0;
        return r;
    }

private:
    UnlockCfg cfg_{};
    bool open_{false};
    int64_t init_[2]{};
    int64_t goal_{0};
    int64_t done_[2]{};
    int64_t pend_[2]{};
    int64_t steps_{0};
    int64_t fills_{0};
    int64_t cancels_{0};
    int64_t rejects_{0};
};

}  // namespace pitlane
