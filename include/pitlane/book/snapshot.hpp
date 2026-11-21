#pragma once

// Full depth snapshot plus the exchange day statistics that travel with it.
// A snapshot feed replaces the whole ladder at once, so this is a plain value
// type: fixed arrays, no allocation, no callbacks. Prices and notionals are
// already fixed point (see util/wirepx.hpp); a zero price means "absent".
//
// The ladder keeps the exchange order (best price first) and drops empty
// levels on load. Bids must descend and asks must ascend; anything else is
// rejected and the previous snapshot stays intact.

#include <cstdint>

namespace pitlane {

inline constexpr int kSnapLevels = 10;

// Wildcards for venue/product filters.
inline constexpr uint8_t kSnapAny = 0xFF;

struct SnapLevel {
    int64_t px{0};
    int64_t qty{0};
};

// Day-scoped statistics. Every field is optional; zero means "not reported".
struct DayBook {
    int64_t pre_close{0};
    int64_t open_px{0};
    int64_t high_px{0};
    int64_t low_px{0};
    int64_t upper_limit{0};
    int64_t lower_limit{0};
    int64_t volume{0};
    int64_t turnover{0};
    int64_t open_interest{0};
    int64_t num_trades{0};
    int64_t total_bid_vol{0};
    int64_t total_ask_vol{0};
    int64_t weight_bid_px{0};
    int64_t weight_ask_px{0};
    int64_t pre_settlement{0};
    int64_t pre_open_interest{0};
    int64_t close_px{0};
    int64_t settlement_px{0};
    int64_t average_px{0};
    int64_t iopv{0};
    int64_t pre_delta{0};
    int64_t curr_delta{0};
    int32_t action_day{0};
    int32_t pre_radio{0};

    bool has_trade() const noexcept { return volume > 0; }
    bool pinned_up(int64_t px) const noexcept { return upper_limit > 0 && px >= upper_limit; }
    bool pinned_down(int64_t px) const noexcept { return lower_limit > 0 && px > 0 && px <= lower_limit; }
};

// Vendor handoff, already decoded. `n_bid`/`n_ask` may include empty slots;
// `load` compacts them.
struct SnapRaw {
    int64_t last_px{0};
    int64_t last_qty{0};
    SnapLevel bid[kSnapLevels]{};
    SnapLevel ask[kSnapLevels]{};
    uint8_t n_bid{0};
    uint8_t n_ask{0};
    uint8_t tagged{1};
    uint8_t status{0};
    int64_t exch_us{0};
    int64_t src_us{0};
    int64_t local_us{0};
};

class SnapBook {
public:
    static constexpr int Levels = kSnapLevels;

    int bid_count() const noexcept { return n_bid; }
    int ask_count() const noexcept { return n_ask; }

    // Whole-snapshot replacement. Fails closed: on a bad ladder neither the
    // book nor the stats change.
    bool load(const SnapRaw& raw, const DayBook& stats) noexcept {
        SnapLevel bid[Levels];
        SnapLevel ask[Levels];
        int nb = 0;
        int na = 0;
        if (!collect(raw.bid, raw.n_bid, true, bid, nb)) return false;
        if (!collect(raw.ask, raw.n_ask, false, ask, na)) return false;
        for (int i = 0; i < nb; ++i) bid_[i] = bid[i];
        for (int i = 0; i < na; ++i) ask_[i] = ask[i];
        n_bid = nb;
        n_ask = na;
        day = stats;
        last_px_ = raw.last_px;
        last_qty_ = raw.last_qty;
        exch_us_ = raw.exch_us;
        src_us_ = raw.src_us;
        local_us_ = raw.local_us;
        tagged_ = raw.tagged;
        status_ = raw.status;
        return true;
    }

    bool load(const SnapRaw& raw) noexcept { return load(raw, DayBook{}); }

    void clear() noexcept {
        n_bid = 0;
        n_ask = 0;
        day = DayBook{};
        last_px_ = 0;
        last_qty_ = 0;
        exch_us_ = 0;
        src_us_ = 0;
        local_us_ = 0;
        tagged_ = 1;
        status_ = 0;
    }

    // Best price first. Returns the number of levels copied.
    int copy(uint8_t side, SnapLevel* out, int cap) const noexcept {
        if (!out || cap <= 0 || (side != 1 && side != 2)) return 0;
        const SnapLevel* src = side == 1 ? bid_ : ask_;
        const int n = side == 1 ? n_bid : n_ask;
        const int take = cap < n ? cap : n;
        for (int i = 0; i < take; ++i) out[i] = src[i];
        return take;
    }

    // Fail-closed when either side is empty.
    bool top(int64_t* bid_px, int64_t* bid_qty, int64_t* ask_px, int64_t* ask_qty) const noexcept {
        if (n_bid <= 0 || n_ask <= 0) return false;
        if (bid_px) *bid_px = bid_[0].px;
        if (bid_qty) *bid_qty = bid_[0].qty;
        if (ask_px) *ask_px = ask_[0].px;
        if (ask_qty) *ask_qty = ask_[0].qty;
        return true;
    }

    int64_t last_px() const noexcept { return last_px_; }
    int64_t last_qty() const noexcept { return last_qty_; }
    int64_t exch_us() const noexcept { return exch_us_; }
    int64_t src_us() const noexcept { return src_us_; }
    int64_t local_us() const noexcept { return local_us_; }
    uint8_t status() const noexcept { return status_; }
    bool tagged() const noexcept { return tagged_ != 0; }

    bool crossed() const noexcept {
        return n_bid > 0 && n_ask > 0 && bid_[0].px >= ask_[0].px;
    }

    // 0 when a side is empty.
    int64_t spread() const noexcept {
        if (n_bid <= 0 || n_ask <= 0) return 0;
        return ask_[0].px - bid_[0].px;
    }

    int64_t mid() const noexcept {
        if (n_bid <= 0 || n_ask <= 0) return 0;
        return (bid_[0].px + ask_[0].px) / 2;
    }

    // Size-weighted mid of the touch, the usual micro-price.
    int64_t micro_px() const noexcept {
        if (n_bid <= 0 || n_ask <= 0) return 0;
        const int64_t total = bid_[0].qty + ask_[0].qty;
        if (total <= 0) return mid();
        return (bid_[0].px * ask_[0].qty + ask_[0].px * bid_[0].qty) / total;
    }

    // (bid - ask) / (bid + ask) in ppm over the first `levels` rungs.
    int64_t imbalance_ppm(int levels) const noexcept {
        const int64_t b = depth_qty(1, levels);
        const int64_t a = depth_qty(2, levels);
        const int64_t total = b + a;
        return total > 0 ? (b - a) * 1000000 / total : 0;
    }

    int64_t depth_qty(uint8_t side, int levels) const noexcept {
        if (side != 1 && side != 2) return 0;
        const SnapLevel* src = side == 1 ? bid_ : ask_;
        const int n = side == 1 ? n_bid : n_ask;
        const int take = levels < n ? levels : n;
        int64_t sum = 0;
        for (int i = 0; i < take; ++i) sum += src[i].qty;
        return sum;
    }

    // Notional / quantity over the first `levels` rungs; 0 when empty.
    int64_t level_vwap(uint8_t side, int levels) const noexcept {
        if (side != 1 && side != 2) return 0;
        const SnapLevel* src = side == 1 ? bid_ : ask_;
        const int n = side == 1 ? n_bid : n_ask;
        const int take = levels < n ? levels : n;
        int64_t qty = 0;
        int64_t notional = 0;
        for (int i = 0; i < take; ++i) {
            qty += src[i].qty;
            notional += src[i].px * src[i].qty;
        }
        return qty > 0 ? notional / qty : 0;
    }

    // Last price pinned at a reported band.
    bool at_limit() const noexcept {
        if (last_px_ <= 0) return false;
        return day.pinned_up(last_px_) || day.pinned_down(last_px_);
    }

    bool stale(int64_t now_us, int64_t max_age_us) const noexcept {
        if (local_us_ <= 0 || max_age_us < 0) return false;
        return now_us > local_us_ && now_us - local_us_ > max_age_us;
    }

    const DayBook& stats() const noexcept { return day; }
    DayBook& stats() noexcept { return day; }

private:
    static bool collect(const SnapLevel* in, int n, bool bid, SnapLevel* out, int& used) noexcept {
        if (n < 0 || n > Levels) return false;
        for (int i = 0; i < n; ++i) {
            if (in[i].px <= 0 || in[i].qty <= 0) continue;
            if (used > 0) {
                const int64_t prev = out[used - 1].px;
                if (bid ? in[i].px >= prev : in[i].px <= prev) return false;
            }
            out[used++] = in[i];
        }
        return true;
    }

    SnapLevel bid_[Levels]{};
    SnapLevel ask_[Levels]{};
    DayBook day{};
    int64_t last_px_{0};
    int64_t last_qty_{0};
    int64_t exch_us_{0};
    int64_t src_us_{0};
    int64_t local_us_{0};
    uint8_t n_bid{0};
    uint8_t n_ask{0};
    uint8_t tagged_{1};
    uint8_t status_{0};
};

}  // namespace pitlane
