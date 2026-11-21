#pragma once

#include <cstdint>

namespace pitlane {

// Full asset and position snapshots as a counter reports them, kept beside the
// live cash and hold lines. Rows are keyed by ledger tier so a product, broker,
// trader, strategy, or merged view can be stored without changing the hot
// order path. Prices use the pitlane 1e-4 scale; cash fields are whole units.

inline constexpr uint32_t kLedgerAny = 0xFFFFFFFFu;
inline constexpr uint8_t kLedgerAnyTier = 0xFF;

enum class LedgerTier : uint8_t {
    Unknown = 0,
    Product = 1,
    Broker = 2,
    Trader = 3,
    Strategy = 4,
    TraderMerge = 5,
    StrategyMerge = 6,
    StrategyByProduct = 7,
};

enum class LedgerSide : uint8_t { Long = 1, Short = 2 };

struct LedgerAsset {
    uint8_t tier{static_cast<uint8_t>(LedgerTier::Unknown)};
    uint8_t last{0};
    uint8_t asset_attribute{0};
    uint8_t currency_type{0};
    uint16_t route_idx{0};
    uint16_t pad{0};
    uint32_t counter{kLedgerAny};
    uint32_t acct{kLedgerAny};
    uint32_t trader{kLedgerAny};
    uint32_t strategy{kLedgerAny};
    uint32_t product{kLedgerAny};

    int64_t initial_equity{0};
    int64_t static_equity{0};
    int64_t dynamic_equity{0};
    int64_t total_cash{0};
    int64_t available_cash{0};
    int64_t market_value{0};
    int64_t margin{0};
    int64_t frozen_cash{0};
    int64_t frozen_margin{0};
    int64_t accumulated_fee{0};
    int64_t intraday_fee{0};
    int64_t frozen_fee{0};
    int64_t realized_pnl{0};
    int64_t unrealized_pnl{0};
    int64_t position_pnl{0};
    int64_t close_pnl{0};
    int64_t credit{0};
    int64_t available_credit{0};
    int64_t asset_balance{0};
    int64_t total_debt{0};
    int64_t fetch_balance{0};
    int64_t fund_debt{0};
    int64_t stock_debt{0};
    int64_t assure_ratio{0};   // parts per million
    int64_t finance_quota{0};
    int64_t shortsell_quota{0};
    int64_t send_us{0};
    int64_t stamp_us{0};
};

struct LedgerPosition {
    uint8_t tier{static_cast<uint8_t>(LedgerTier::Unknown)};
    uint8_t direction{static_cast<uint8_t>(LedgerSide::Long)};
    uint8_t instrument_type{0};
    uint8_t currency_type{0};
    uint8_t last{0};
    uint8_t pad[3]{};
    uint16_t route_idx{0};
    uint16_t pad2{0};
    uint32_t counter{kLedgerAny};
    uint32_t acct{kLedgerAny};
    uint32_t trader{kLedgerAny};
    uint32_t strategy{kLedgerAny};
    uint32_t product{kLedgerAny};
    uint32_t name{kLedgerAny};

    int64_t total{0};
    int64_t initial_yesterday{0};
    int64_t left_yesterday{0};
    int64_t sellable{0};
    int64_t frozen{0};
    int64_t frozen_yesterday{0};
    int64_t yesterday_buy{0};
    int64_t today_buy{0};
    int64_t yesterday_sell{0};
    int64_t today_sell{0};
    int64_t entrust_buy{0};
    int64_t entrust_sell{0};

    int64_t last_px{0};
    int64_t avg_open_px{0};
    int64_t cost_px{0};
    int64_t close_px{0};
    int64_t pre_close_px{0};
    int64_t settlement_px{0};
    int64_t pre_settlement_px{0};
    int64_t margin{0};
    int64_t position_pnl{0};
    int64_t close_pnl{0};
    int64_t realized_pnl{0};
    int64_t unrealized_pnl{0};
    int64_t commission{0};
    int64_t stamp_us{0};
};

struct LedgerFilter {
    uint8_t tier{kLedgerAnyTier};
    uint8_t direction{0};
    uint32_t counter{kLedgerAny};
    uint32_t acct{kLedgerAny};
    uint32_t trader{kLedgerAny};
    uint32_t strategy{kLedgerAny};
    uint32_t product{kLedgerAny};
    uint32_t name{kLedgerAny};
};

namespace ledger_detail {

inline void zero(uint32_t& v) noexcept { v = 0; }

inline void normalize_keys(uint8_t tier, uint32_t& counter, uint32_t& acct, uint32_t& trader,
                           uint32_t& strategy) noexcept {
    switch (static_cast<LedgerTier>(tier)) {
        case LedgerTier::Product:
            zero(counter);
            zero(acct);
            zero(trader);
            zero(strategy);
            break;
        case LedgerTier::Broker:
            zero(trader);
            zero(strategy);
            break;
        case LedgerTier::Trader:
            zero(strategy);
            break;
        case LedgerTier::TraderMerge:
            zero(counter);
            zero(acct);
            zero(strategy);
            break;
        case LedgerTier::StrategyMerge:
            zero(counter);
            zero(acct);
            break;
        case LedgerTier::StrategyByProduct:
            zero(counter);
            zero(acct);
            zero(trader);
            break;
        default:
            break;
    }
}

inline bool match(uint8_t tier, uint32_t counter, uint32_t acct, uint32_t trader, uint32_t strategy,
                  uint32_t product, const LedgerFilter& f) noexcept {
    return (f.tier == kLedgerAnyTier || tier == f.tier) &&
           (f.counter == kLedgerAny || counter == f.counter) &&
           (f.acct == kLedgerAny || acct == f.acct) &&
           (f.trader == kLedgerAny || trader == f.trader) &&
           (f.strategy == kLedgerAny || strategy == f.strategy) &&
           (f.product == kLedgerAny || product == f.product);
}

}  // namespace ledger_detail

// Fixed snapshot table. Writes are cold and replace in place; reads walk the
// insertion order and return a stable cursor, so a console can page a large
// account without copying or sorting the whole set.
template <int Cap>
class LedgerView {
public:
    static_assert(Cap >= 1, "ledger capacity");

    bool upsert(const LedgerAsset& row_in) noexcept {
        LedgerAsset row = row_in;
        ledger_detail::normalize_keys(row.tier, row.counter, row.acct, row.trader, row.strategy);
        for (int i = 0; i < asset_n_; ++i) {
            if (same_asset(asset_[i], row)) {
                asset_[i] = row;
                return true;
            }
        }
        if (asset_n_ >= Cap) return false;
        asset_[asset_n_++] = row;
        return true;
    }

    bool upsert(const LedgerPosition& row_in) noexcept {
        LedgerPosition row = row_in;
        ledger_detail::normalize_keys(row.tier, row.counter, row.acct, row.trader, row.strategy);
        for (int i = 0; i < position_n_; ++i) {
            if (same_position(position_[i], row)) {
                position_[i] = row;
                return true;
            }
        }
        if (position_n_ >= Cap) return false;
        position_[position_n_++] = row;
        return true;
    }

    // Copies at most cap matching assets starting at cursor. next receives the
    // index to pass on the following page; is_last is true when no matching
    // row remains. The final returned row carries last = 1.
    int query_assets(const LedgerFilter& f, int cursor, LedgerAsset* out, int cap, int* next,
                     bool* is_last) const noexcept {
        if (!out || cap <= 0) {
            if (next) *next = cursor < 0 ? 0 : cursor;
            if (is_last) *is_last = asset_n_ == 0;
            return 0;
        }
        int i = cursor < 0 ? 0 : cursor;
        int written = 0;
        while (i < asset_n_ && written < cap) {
            const LedgerAsset& row = asset_[i++];
            if (ledger_detail::match(row.tier, row.counter, row.acct, row.trader, row.strategy,
                                     row.product, f)) {
                out[written] = row;
                out[written].last = 0;
                ++written;
            }
        }
        bool last = true;
        for (int j = i; j < asset_n_; ++j) {
            const LedgerAsset& row = asset_[j];
            if (ledger_detail::match(row.tier, row.counter, row.acct, row.trader, row.strategy,
                                     row.product, f)) {
                last = false;
                break;
            }
        }
        if (written) out[written - 1].last = last ? 1 : 0;
        if (next) *next = i;
        if (is_last) *is_last = last;
        return written;
    }

    int query_positions(const LedgerFilter& f, int cursor, LedgerPosition* out, int cap, int* next,
                        bool* is_last) const noexcept {
        if (!out || cap <= 0) {
            if (next) *next = cursor < 0 ? 0 : cursor;
            if (is_last) *is_last = position_n_ == 0;
            return 0;
        }
        int i = cursor < 0 ? 0 : cursor;
        int written = 0;
        while (i < position_n_ && written < cap) {
            const LedgerPosition& row = position_[i++];
            if (match_position(row, f)) {
                out[written] = row;
                out[written].last = 0;
                ++written;
            }
        }
        bool last = true;
        for (int j = i; j < position_n_; ++j) {
            if (match_position(position_[j], f)) {
                last = false;
                break;
            }
        }
        if (written) out[written - 1].last = last ? 1 : 0;
        if (next) *next = i;
        if (is_last) *is_last = last;
        return written;
    }

    // Sums every matching asset row into one total. Useful for a parent tier:
    // fold all strategy rows to get the trader view, then all trader rows for
    // the broker view.
    LedgerAsset sum_assets(const LedgerFilter& f) const noexcept {
        LedgerAsset total{};
        total.tier = f.tier == kLedgerAnyTier ? static_cast<uint8_t>(LedgerTier::Unknown) : f.tier;
        for (int i = 0; i < asset_n_; ++i) {
            const LedgerAsset& row = asset_[i];
            if (!ledger_detail::match(row.tier, row.counter, row.acct, row.trader, row.strategy,
                                      row.product, f)) {
                continue;
            }
            total.initial_equity += row.initial_equity;
            total.static_equity += row.static_equity;
            total.dynamic_equity += row.dynamic_equity;
            total.total_cash += row.total_cash;
            total.available_cash += row.available_cash;
            total.market_value += row.market_value;
            total.margin += row.margin;
            total.frozen_cash += row.frozen_cash;
            total.frozen_margin += row.frozen_margin;
            total.accumulated_fee += row.accumulated_fee;
            total.intraday_fee += row.intraday_fee;
            total.frozen_fee += row.frozen_fee;
            total.realized_pnl += row.realized_pnl;
            total.unrealized_pnl += row.unrealized_pnl;
            total.position_pnl += row.position_pnl;
            total.close_pnl += row.close_pnl;
            total.credit += row.credit;
            total.available_credit += row.available_credit;
            total.asset_balance += row.asset_balance;
            total.total_debt += row.total_debt;
            total.fetch_balance += row.fetch_balance;
            total.fund_debt += row.fund_debt;
            total.stock_debt += row.stock_debt;
            total.finance_quota += row.finance_quota;
            total.shortsell_quota += row.shortsell_quota;
            if (row.send_us > total.send_us) total.send_us = row.send_us;
            if (row.stamp_us > total.stamp_us) total.stamp_us = row.stamp_us;
        }
        return total;
    }

    int assets() const noexcept { return asset_n_; }
    int positions() const noexcept { return position_n_; }

    void clear() noexcept {
        asset_n_ = 0;
        position_n_ = 0;
    }

    void clear_assets() noexcept { asset_n_ = 0; }
    void clear_positions() noexcept { position_n_ = 0; }

private:
    static bool same_asset(const LedgerAsset& a, const LedgerAsset& b) noexcept {
        return a.tier == b.tier && a.counter == b.counter && a.acct == b.acct &&
               a.trader == b.trader && a.strategy == b.strategy && a.product == b.product;
    }

    static bool same_position(const LedgerPosition& a, const LedgerPosition& b) noexcept {
        return a.tier == b.tier && a.direction == b.direction && a.counter == b.counter &&
               a.acct == b.acct && a.trader == b.trader && a.strategy == b.strategy &&
               a.product == b.product && a.name == b.name;
    }

    static bool match_position(const LedgerPosition& row, const LedgerFilter& f) noexcept {
        return ledger_detail::match(row.tier, row.counter, row.acct, row.trader, row.strategy,
                                    row.product, f) &&
               (f.name == kLedgerAny || row.name == f.name) &&
               (f.direction == 0 || row.direction == f.direction);
    }

    int asset_n_{0};
    int position_n_{0};
    LedgerAsset asset_[Cap]{};
    LedgerPosition position_[Cap]{};
};

}  // namespace pitlane
