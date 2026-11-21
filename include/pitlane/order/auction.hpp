#pragma once

// Call-auction execution. One parent quantity is worked during the opening or
// closing auction with a single child at a time:
//
//   before the window            -> wait
//   inside the window            -> place the remainder once, then wait
//   child rejected or cancelled  -> replace with a fresh id
//   past the window              -> hand the remainder on, or stop
//
// Pricing follows `px_mode`: a specified limit aligned to the tick grid, the
// opposite touch walked by `adjust_ticks`, or the mid aligned to the aggressive
// side. The grid is `book/steps.hpp`, so banded ticks and the daily price band
// apply without extra state.
//
// `allow_at_limit` is the board policy: with it cleared, a buy stops when the
// ask reaches the down limit and a sell stops when the bid reaches the up
// limit. The stop is latched, like an exchange-side halt.
//
// All state is a fixed set of integers. Nothing allocates and nothing locks.

#include "pitlane/book/steps.hpp"
#include "pitlane/order/algo.hpp"
#include "pitlane/order/quoteprice.hpp"

#include <cstdint>

namespace pitlane {

enum class AucState : uint8_t {
    Idle = 0,
    Wait = 1,
    Working = 2,
    Roll = 3,
    Done = 4,
    Stopped = 5,
    Failed = 6,
};

inline const char* auc_state_name(uint8_t s) noexcept {
    switch (static_cast<AucState>(s)) {
        case AucState::Wait: return "wait";
        case AucState::Working: return "working";
        case AucState::Roll: return "roll";
        case AucState::Done: return "done";
        case AucState::Stopped: return "stopped";
        case AucState::Failed: return "failed";
        default: return "idle";
    }
}

enum : uint8_t { kAucOpen = 1, kAucClose = 2 };
enum : uint8_t { kAucTouch = 0, kAucLimit = 1, kAucMid = 2 };

struct AucCfg {
    uint32_t name{0};
    uint8_t side{1};            // Side
    uint8_t kind{kAucOpen};     // kAucOpen / kAucClose
    uint8_t px_mode{kAucTouch}; // kAucTouch / kAucLimit / kAucMid
    uint8_t undo{1};            // hand the remainder on after the auction
    uint8_t allow_at_limit{0};  // 0 = stop when the market is pinned at the band
    uint8_t pad[2]{};
    int64_t total{0};
    int64_t begin_us{0};
    int64_t end_us{0};
    int64_t lot{1};
    int64_t tick{1};
    int64_t limit_px{0};     // required by kAucLimit
    int64_t adjust_ticks{0}; // touch and mid only, measured in ticks
    int64_t max_clip{0};     // 0 = the whole remainder
    int64_t up_limit{0};     // 0 = unknown
    int64_t down_limit{0};   // 0 = unknown
    uint64_t id_base{1};
    const PxBand* bands{nullptr};
    int band_n{0};
};

struct AucRow {
    uint8_t state{0};
    uint8_t working{0};
    uint8_t reason{0}; // 0 normal, 1 at the band, 2 replaced, 3 auction over
    int64_t total{0};
    int64_t done{0};
    int64_t working_qty{0};
    int64_t remaining{0};
    int64_t avg_px{0};
    uint64_t sends{0};
    uint64_t fills{0};
    uint64_t cancels{0};
    uint64_t rejects{0};
};

class AuctionAlgo {
public:
    AucState state() const noexcept { return static_cast<AucState>(st_); }
    bool active() const noexcept { return st_ == static_cast<uint8_t>(AucState::Wait) || st_ == static_cast<uint8_t>(AucState::Working); }
    bool working() const noexcept { return working_ != 0; }
    int64_t done() const noexcept { return done_; }
    int64_t remaining() const noexcept { return cfg_.total > done_ ? cfg_.total - done_ : 0; }

    bool open(const AucCfg& cfg, int64_t now_us) noexcept {
        cfg_ = cfg;
        st_ = static_cast<uint8_t>(AucState::Idle);
        paused_ = 0;
        working_ = 0;
        limit_hit_ = 0;
        has_quote_ = 0;
        roll_taken_ = 0;
        reason_ = 0;
        done_ = 0;
        filled_notional_ = 0;
        working_id_ = 0;
        working_qty_ = 0;
        sends_ = fills_ = cancels_ = rejects_ = 0;
        quote_ = QuoteTop{};
        if (cfg_.name == 0 || (cfg_.side != static_cast<uint8_t>(Side::Buy) && cfg_.side != static_cast<uint8_t>(Side::Sell))) {
            st_ = static_cast<uint8_t>(AucState::Failed);
            return false;
        }
        if (cfg_.kind != kAucOpen && cfg_.kind != kAucClose) {
            st_ = static_cast<uint8_t>(AucState::Failed);
            return false;
        }
        if (cfg_.px_mode != kAucTouch && cfg_.px_mode != kAucLimit && cfg_.px_mode != kAucMid) {
            st_ = static_cast<uint8_t>(AucState::Failed);
            return false;
        }
        if (cfg_.total <= 0 || cfg_.lot <= 0 || cfg_.tick <= 0 || cfg_.end_us <= cfg_.begin_us) {
            st_ = static_cast<uint8_t>(AucState::Failed);
            return false;
        }
        if (cfg_.adjust_ticks < 0 || cfg_.max_clip < 0 || cfg_.up_limit < 0 || cfg_.down_limit < 0) {
            st_ = static_cast<uint8_t>(AucState::Failed);
            return false;
        }
        if (cfg_.up_limit > 0 && cfg_.down_limit > 0 && cfg_.up_limit <= cfg_.down_limit) {
            st_ = static_cast<uint8_t>(AucState::Failed);
            return false;
        }
        if (cfg_.px_mode == kAucLimit && cfg_.limit_px <= 0) {
            st_ = static_cast<uint8_t>(AucState::Failed);
            return false;
        }
        PxBand one{0, INT64_MAX, cfg_.tick};
        const PxBand* bands = cfg_.bands;
        int count = cfg_.band_n;
        if (!bands || count <= 0) {
            bands = &one;
            count = 1;
        }
        if (!grid_.load(bands, count, 0, 0)) {
            st_ = static_cast<uint8_t>(AucState::Failed);
            return false;
        }
        st_ = now_us < cfg_.begin_us ? static_cast<uint8_t>(AucState::Wait) : static_cast<uint8_t>(AucState::Working);
        return true;
    }

    void on_quote(const QuoteTop& q, int64_t now_us) noexcept {
        (void)now_us;
        quote_ = q;
        has_quote_ = 1;
        if (!active() || cfg_.allow_at_limit || limit_hit_) return;
        if (cfg_.side == static_cast<uint8_t>(Side::Buy)) {
            if (cfg_.down_limit > 0 && q.ask[0] > 0 && q.ask[0] <= cfg_.down_limit) limit_hit_ = 1;
        } else {
            if (cfg_.up_limit > 0 && q.bid[0] > 0 && q.bid[0] >= cfg_.up_limit) limit_hit_ = 1;
        }
    }

    int poll(int64_t now_us, ExecCmd* out, int cap) noexcept {
        if (!out || cap <= 0 || paused_) return 0;
        if (st_ == static_cast<uint8_t>(AucState::Wait)) {
            if (now_us < cfg_.begin_us) return 0;
            st_ = static_cast<uint8_t>(AucState::Working);
        }
        if (st_ != static_cast<uint8_t>(AucState::Working)) return 0;
        if (limit_hit_) {
            reason_ = 1;
            st_ = static_cast<uint8_t>(AucState::Stopped);
            return 0;
        }
        if (done_ >= cfg_.total) {
            st_ = static_cast<uint8_t>(AucState::Done);
            return 0;
        }
        if (now_us > cfg_.end_us) {
            reason_ = 3;
            if (cfg_.undo) {
                st_ = static_cast<uint8_t>(AucState::Roll);
            } else {
                st_ = static_cast<uint8_t>(AucState::Stopped);
            }
            return 0;
        }
        if (working_ || !has_quote_) return 0;
        const int64_t qty = clip_qty();
        if (qty <= 0) return 0;
        const bool buy = cfg_.side == static_cast<uint8_t>(Side::Buy);
        const int64_t px = price_now(buy);
        if (px <= 0) return 0;
        const uint64_t id = cfg_.id_base + sends_;
        if (id == 0 || id < cfg_.id_base) {
            st_ = static_cast<uint8_t>(AucState::Failed);
            return 0;
        }
        ExecCmd& c = out[0];
        c = ExecCmd{};
        c.kind = kExecPlace;
        c.side = cfg_.side;
        c.style = static_cast<uint8_t>(Style::Immediate);
        c.reason = 0;
        c.id = id;
        c.px = px;
        c.qty = qty;
        ++sends_;
        working_ = 1;
        working_id_ = id;
        working_qty_ = qty;
        return 1;
    }

    void on_ack(uint64_t id) noexcept { (void)id; }

    void on_fill(int64_t qty, int64_t px) noexcept {
        if (qty <= 0) return;
        int64_t take = qty;
        if (working_qty_ > 0 && take > working_qty_) take = working_qty_;
        working_qty_ -= take;
        if (working_qty_ <= 0) {
            working_qty_ = 0;
            working_ = 0;
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
            working_ = 0;
            working_qty_ = 0;
            working_id_ = 0;
            st_ = static_cast<uint8_t>(AucState::Done);
        }
    }

    void on_cancel() noexcept {
        ++cancels_;
        working_ = 0;
        working_qty_ = 0;
        working_id_ = 0;
        if (st_ == static_cast<uint8_t>(AucState::Working)) reason_ = 2;
    }

    void on_reject() noexcept {
        ++rejects_;
        working_ = 0;
        working_qty_ = 0;
        working_id_ = 0;
        if (st_ == static_cast<uint8_t>(AucState::Working)) reason_ = 2;
    }

    bool pause() noexcept {
        if (!active() || paused_) return false;
        paused_ = 1;
        return true;
    }

    bool resume() noexcept {
        if (!paused_) return false;
        paused_ = 0;
        return true;
    }

    bool stop() noexcept {
        if (!active()) return false;
        st_ = static_cast<uint8_t>(AucState::Stopped);
        paused_ = 0;
        return true;
    }

    // Only valid in Roll. Returns the untouched quantity and ends the auction.
    int64_t roll_out() noexcept {
        if (st_ != static_cast<uint8_t>(AucState::Roll) || roll_taken_) return 0;
        roll_taken_ = 1;
        const int64_t left = remaining();
        st_ = static_cast<uint8_t>(AucState::Done);
        return left;
    }

    AucRow row() const noexcept {
        AucRow r{};
        r.state = st_;
        r.working = working_;
        r.reason = reason_;
        r.total = cfg_.total;
        r.done = done_;
        r.working_qty = working_qty_;
        r.remaining = remaining();
        r.avg_px = done_ > 0 ? filled_notional_ / done_ : 0;
        r.sends = sends_;
        r.fills = fills_;
        r.cancels = cancels_;
        r.rejects = rejects_;
        return r;
    }

private:
    int64_t clip_qty() const noexcept {
        int64_t qty = remaining();
        if (cfg_.max_clip > 0 && qty > cfg_.max_clip) qty = cfg_.max_clip;
        qty = (qty / cfg_.lot) * cfg_.lot;
        return qty > 0 ? qty : 0;
    }

    int64_t clamp_band(int64_t px) const noexcept {
        if (px <= 0) return px;
        if (cfg_.down_limit > 0 && px < cfg_.down_limit) px = cfg_.down_limit;
        if (cfg_.up_limit > 0 && px > cfg_.up_limit) px = cfg_.up_limit;
        return px;
    }

    int64_t price_now(bool buy) const noexcept {
        if (cfg_.px_mode == kAucLimit) {
            const int64_t px = buy ? grid_.ceil_px(cfg_.limit_px) : grid_.floor_px(cfg_.limit_px);
            return clamp_band(px);
        }
        const int mode = cfg_.px_mode == kAucMid ? 2 : 0;
        const int64_t raw = pre_px(buy, mode, quote_, false, false);
        if (raw <= 0) return 0;
        int64_t px = buy ? grid_.ceil_px(raw) : grid_.floor_px(raw);
        if (cfg_.adjust_ticks > 0) {
            const int steps = cfg_.adjust_ticks > 1000000 ? 1000000 : static_cast<int>(cfg_.adjust_ticks);
            px = buy ? grid_.walk_up(px, steps) : grid_.walk_down(px, steps);
        }
        return clamp_band(px);
    }

    AucCfg cfg_{};
    QuoteTop quote_{};
    TickGrid grid_{};
    uint8_t st_{0};
    uint8_t paused_{0};
    uint8_t working_{0};
    uint8_t limit_hit_{0};
    uint8_t has_quote_{0};
    uint8_t roll_taken_{0};
    uint8_t reason_{0};
    int64_t done_{0};
    int64_t filled_notional_{0};
    uint64_t working_id_{0};
    int64_t working_qty_{0};
    uint64_t sends_{0};
    uint64_t fills_{0};
    uint64_t cancels_{0};
    uint64_t rejects_{0};
};

}  // namespace pitlane
