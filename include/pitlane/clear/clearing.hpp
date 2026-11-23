#pragma once

// Clearing, valuation, and reconciliation.
//
// Fills open FIFO lots; a close realises PnL against the oldest lot of the
// opposite side, so yesterday's lots are consumed before today's. Lots opened
// since the last settle carry the `today` flag, which selects the close-today
// fee rate and feeds the today/yesterday quantity split.
//
// Two valuation bases are kept per lot: the trade price (`px`, close basis)
// and the last settlement price (`settle_px`, settlement basis). `settle()`
// marks open lots to the settlement price and books that mark-to-market move as
// settled PnL, then resets the basis, which is how a futures book carries PnL
// into the next day.
//
// Cash is an opening balance plus a flow and income/expense ledger:
// deposits, withdrawals, dividends, accrued fees, financing interest, and share
// subscriptions/redemptions. Equity is the cash balance plus realised and
// unrealised PnL less trading fees; `equity_settle` values the book on the
// settlement basis. Shares turn equity into a unit NAV with a peak and a
// drawdown; a cumulative NAV keeps distributions in the track record.
//
// Corporate actions (cash dividends with withholding, stock dividends, splits
// and merges) are applied against the lots held before the ex-date and are
// idempotent by (name, event_id).
//
// The book is owned by the trading thread; the web console only reads
// ClearingBoard snapshots.

#include "pitlane/order/pms.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

inline constexpr int64_t kClrPxScale = 10000;   // prices are fixed point 1e-4
inline constexpr int64_t kClrPpm = 1000000;
inline constexpr int64_t kClrNavScale = 1000000;  // unit NAV is ppm of one unit
inline constexpr int kClrMaxCcy = 8;
inline constexpr int kClrMaxDays = 32;

enum ClrCashKind : uint8_t {
    kClrDeposit = 1,
    kClrWithdraw = 2,
    kClrDividend = 3,
    kClrFee = 4,
    kClrInterest = 5,
    kClrSubscribe = 6,
    kClrRedeem = 7,
};

// Buckets for accrued expense, so the report can split management, custody,
// sales, and other fees without a second ledger.
enum ClrExpenseKind : uint8_t {
    kClrMgmtFee = 1,
    kClrCustodyFee = 2,
    kClrSalesFee = 3,
    kClrOtherFee = 4,
};

enum ClrCorpKind : uint8_t {
    kClrCashDiv = 1,
    kClrStockDiv = 2,
    kClrSplit = 3,
    kClrMerge = 4,
};

// One corporate action. `per10_cash` is cash per ten shares in 1e-4 currency
// units, `per10_stock` is bonus shares per ten shares, and `num`/`den` scale a
// split or merge (`new = old * num / den`).
struct ClrCorp {
    uint32_t name{0};
    int64_t event_id{0};
    uint8_t kind{kClrCashDiv};
    int64_t per10_cash{0};
    int64_t per10_stock{0};
    int64_t num{1};
    int64_t den{1};
    int64_t tax_ppm{100000};
};

struct ClrCorpOut {
    int64_t cash{0};  // net cash credited, whole currency units (negative when payable)
    int64_t tax{0};
    int64_t qty_added{0};
    int64_t qty_removed{0};
    bool applied{false};
    bool short_blocked{false};
};

struct ClrCcy {
    char code[8]{};
    int64_t fx_ppm{kClrPpm};  // base currency units per one unit, ppm
};

// One cash movement (deposit or withdrawal) in the ledger.
struct ClrCashMove {
    int64_t seq{0};
    int64_t stamp_us{0};
    uint8_t kind{kClrDeposit};
    uint8_t ccy{0};
    int64_t amount{0};       // local currency, whole units
    int64_t base_amount{0};  // converted to base currency at booking time
};

// A settled day, kept so the console can show history and the desk can audit.
struct ClrDayStat {
    uint32_t day_seq{0};
    int64_t settle_us{0};
    int64_t equity{0};
    int64_t cash_balance{0};
    int64_t unit_nav{0};
    int64_t realized{0};    // day PnL realised on the close basis
    int64_t settled{0};     // mark-to-market booked at this settle
    int64_t fees{0};
    int64_t trades{0};
    int64_t turnover{0};
    int64_t deposits{0};    // cumulative
    int64_t withdrawals{0};
    int64_t pnl{0};         // day PnL net of deposits, withdrawals, and share movements
    int64_t income{0};      // cumulative
    int64_t expense{0};     // cumulative
    int64_t cum_nav{0};     // unit NAV plus distributions
};

// Period sums over the day ring, computed on read.
struct ClrPeriod {
    int64_t today{0};
    int64_t d7{0};
    int64_t d30{0};
    int64_t ytd{0};
};

struct ClrLot {
    uint32_t name{0};
    int32_t open_id{0};
    int8_t side{1};  // +1 long, -1 short
    int64_t qty{0};
    int64_t px{0};         // trade price, close basis
    int64_t settle_px{0};  // settlement basis, starts at px
    int64_t mult{1};
    int64_t margin_ppm{0};
    uint8_t ccy{0};
    bool today{true};
};

struct ClrFillIn {
    uint32_t acct{0};
    uint32_t name{0};
    uint8_t side{0};  // 1 buy, 2 sell
    int64_t qty{0};
    int64_t px{0};
    int64_t mult{1};
    int64_t margin_ppm{0};
    int64_t fee_ppm{0};               // open commission rate
    int64_t close_fee_ppm{-1};        // closing yesterday; <0 uses fee_ppm
    int64_t close_today_fee_ppm{-1};  // closing today; <0 uses close fee
    int64_t stamp_ppm{0};
    int64_t min_fee{0};
    bool allow_short{false};
    uint8_t ccy{0};
};

struct ClrFillOut {
    int64_t realized{0};         // close basis
    int64_t realized_settle{0};  // settlement basis
    int64_t fee{0};
    int64_t commission{0};
    int64_t stamp{0};
    int64_t open_fee{0};
    int64_t close_fee{0};
    int64_t closed_qty{0};
    int64_t closed_today{0};
    int64_t closed_yday{0};
    int64_t opened_qty{0};
};

// One booked trade, kept so the desk can audit what the book saw.
struct ClrTrade {
    int64_t seq{0};
    int64_t stamp_us{0};
    uint32_t name{0};
    uint8_t side{0};
    int64_t qty{0};
    int64_t px{0};
    int64_t fee{0};
    int64_t realized{0};
    int64_t realized_settle{0};
    int64_t closed_today{0};
    int64_t closed_yday{0};
};

struct ClearingRecon {
    uint32_t name{0};
    int64_t clr_qty{0};
    int64_t pms_qty{0};
    int64_t diff{0};
};

// A mark per name. `last` values the book on the close basis and supplies the
// market value; `settle` is the settlement price (0 falls back to `last`).
struct ClrMark {
    uint32_t name{0};
    int64_t last{0};
    int64_t settle{0};
};

struct ClearingRow {
    uint32_t name{0};
    uint8_t ccy{0};
    int64_t fx_ppm{kClrPpm};
    int64_t long_qty{0};
    int64_t short_qty{0};
    int64_t long_today{0};
    int64_t long_yday{0};
    int64_t short_today{0};
    int64_t short_yday{0};
    int64_t avg_long{0};
    int64_t avg_short{0};
    int64_t realized{0};
    int64_t realized_settle{0};
    int64_t unrealized{0};
    int64_t unrealized_settle{0};
    int64_t margin{0};
    int64_t market_value{0};
    int64_t fees{0};
    int64_t commission{0};
    int64_t stamp{0};
    int64_t trades{0};
    int64_t turnover{0};
    int64_t mark{0};
    int64_t settle_mark{0};
};

struct ClearingReport {
    uint64_t seq{0};
    int64_t stamp_us{0};
    uint32_t acct{0};
    uint8_t env{0};
    int32_t rows{0};
    int64_t realized{0};
    int64_t unrealized{0};
    int64_t fees{0};
    int64_t margin{0};
    int64_t market_value{0};
    int64_t cash_in{0};     // opening cash of the funding account
    int64_t cash_flow{0};   // realized - fees
    int64_t equity{0};      // cash_balance + cash_flow + unrealized
    int64_t available{0};   // equity - margin
    int64_t commission{0};
    int64_t stamp{0};
    int64_t trades{0};
    int64_t turnover{0};
    int64_t day_realized{0};
    int64_t day_fees{0};
    int64_t day_trades{0};
    int64_t day_turnover{0};
    int64_t settle_us{0};
    uint32_t day_seq{0};
    // Cash ledger, dual valuation, PnL and NAV.
    int64_t deposits{0};
    int64_t withdrawals{0};
    int64_t cash_balance{0};
    int64_t realized_settle{0};
    int64_t unrealized_settle{0};
    int64_t equity_settle{0};
    int64_t pre_equity{0};
    int64_t today_pnl{0};
    int64_t total_pnl{0};
    int64_t day_settled{0};
    int64_t shares{0};
    int64_t unit_nav{0};
    int64_t peak_nav{0};
    int64_t drawdown_ppm{0};
    int64_t cumulative_nav{0};
    int64_t distributions_ppm{0};
    int64_t income{0};
    int64_t expense{0};
    int64_t mgmt_fee{0};
    int64_t custody_fee{0};
    int64_t sales_fee{0};
    int64_t fin_interest{0};
    int64_t shares_in{0};
    int64_t shares_out{0};
    int64_t net_flow{0};  // cumulative deposits less withdrawals, base currency
    int32_t fx_missing{0};
    int32_t days{0};
    ClrDayStat day[kClrMaxDays]{};
    int32_t moves{0};
    ClrCashMove move[8]{};
    ClearingRow row[8]{};
    int64_t sum_long() const noexcept {
        int64_t v = 0;
        for (int i = 0; i < rows; ++i) v += row[i].long_qty;
        return v;
    }
    int64_t sum_short() const noexcept {
        int64_t v = 0;
        for (int i = 0; i < rows; ++i) v += row[i].short_qty;
        return v;
    }
    int32_t recon_rows{0};
    int32_t recon_diff_rows{0};
    ClearingRecon recon[8]{};
};

class ClearingBook {
public:
    static constexpr int kMaxLots = 64;
    static constexpr int kMaxNames = 8;

    void reset(uint32_t acct, uint8_t env) noexcept {
        acct_ = acct;
        env_ = env;
        n_lots_ = 0;
        n_names_ = 0;
        next_open_id_ = 1;
        commission_ = 0;
        stamp_ = 0;
        trade_count_ = 0;
        turnover_ = 0;
        day_realized_ = 0;
        day_fees_ = 0;
        day_trades_ = 0;
        day_turnover_ = 0;
        day_settled_ = 0;
        settle_us_ = 0;
        day_seq_ = 0;
        n_trades_ = 0;
        trade_head_ = 0;
        trade_seq_ = 0;
        cash_in_ = 0;
        deposits_ = 0;
        withdrawals_ = 0;
        flow_since_settle_ = 0;
        pre_equity_ = 0;
        has_pre_ = false;
        shares_ = 0;
        peak_nav_ = 0;
        last_equity_ = 0;
        last_nav_ = 0;
        has_nav_ = false;
        income_ = 0;
        expense_ = 0;
        mgmt_fee_ = 0;
        custody_fee_ = 0;
        sales_fee_ = 0;
        fin_interest_ = 0;
        shares_in_ = 0;
        shares_out_ = 0;
        subs_ = 0;
        redemptions_ = 0;
        distributions_ppm_ = 0;
        n_corp_ = 0;
        corp_head_ = 0;
        for (int i = 0; i < kCorpSeen; ++i) corp_[i] = CorpSeen{};
        n_cash_ = 0;
        cash_head_ = 0;
        cash_seq_ = 0;
        day_head_ = 0;
        day_count_ = 0;
        for (int i = 0; i < kClrMaxCcy; ++i) ccy_[i] = ClrCcy{};
        std::snprintf(ccy_[0].code, sizeof ccy_[0].code, "CNY");
        ccy_[0].fx_ppm = kClrPpm;
        for (int i = 0; i < kMaxNames; ++i) {
            last_mark_[i] = 0;
            last_settle_mark_[i] = 0;
        }
        for (int i = 0; i < kClrMaxDays; ++i) days_[i] = ClrDayStat{};
        for (int i = 0; i < kMaxCash; ++i) cash_[i] = ClrCashMove{};
    }

    uint32_t acct() const noexcept { return acct_; }

    void set_env(uint8_t env) noexcept { env_ = env; }

    // Opening cash of the funding account, in base currency units.
    void set_cash(int64_t cash) noexcept {
        cash_in_ = cash;
        if (!has_pre_) {
            pre_equity_ = cash;
            has_pre_ = true;
        }
    }

    int64_t cash_in() const noexcept { return cash_in_; }

    // ---- currency table --------------------------------------------------

    void set_base_ccy(const char* code) noexcept {
        if (!code || !code[0]) return;
        std::snprintf(ccy_[0].code, sizeof ccy_[0].code, "%s", code);
        ccy_[0].fx_ppm = kClrPpm;
    }

    const char* base_ccy() const noexcept { return ccy_[0].code; }

    // Registers a currency code and returns its slot, or -1 when the table is
    // full. A new code starts at parity until set_rate says otherwise.
    int ccy_id(const char* code) noexcept {
        if (!code || !code[0]) return 0;
        for (int i = 0; i < kClrMaxCcy; ++i) {
            if (ccy_[i].code[0] && std::strcmp(ccy_[i].code, code) == 0) return i;
        }
        for (int i = 0; i < kClrMaxCcy; ++i) {
            if (!ccy_[i].code[0]) {
                std::snprintf(ccy_[i].code, sizeof ccy_[i].code, "%s", code);
                ccy_[i].fx_ppm = kClrPpm;
                return i;
            }
        }
        return -1;
    }

    void set_rate(uint8_t ccy, int64_t fx_ppm) noexcept {
        if (ccy < kClrMaxCcy) ccy_[ccy].fx_ppm = fx_ppm;
    }

    int64_t rate(uint8_t ccy) const noexcept {
        return ccy < kClrMaxCcy ? ccy_[ccy].fx_ppm : 0;
    }

    const char* ccy_code(uint8_t ccy) const noexcept {
        return ccy < kClrMaxCcy ? ccy_[ccy].code : "";
    }

    // ---- cash ledger -----------------------------------------------------

    // Cash is the opening balance plus deposits less withdrawals plus income less expense.
    // Share subscriptions and redemptions move cash: subscribing brings cash
    // in, redeeming pays it out, and neither is PnL.
    int64_t cash_balance() const noexcept {
        return cash_in_ + deposits_ - withdrawals_ + income_ - expense_ + subs_ - redemptions_;
    }
    int64_t deposits() const noexcept { return deposits_; }
    int64_t withdrawals() const noexcept { return withdrawals_; }

    ClrCashMove deposit(int64_t amount, uint8_t ccy, int64_t stamp_us) noexcept {
        return book_cash(kClrDeposit, amount, ccy, stamp_us);
    }

    ClrCashMove withdraw(int64_t amount, uint8_t ccy, int64_t stamp_us) noexcept {
        return book_cash(kClrWithdraw, amount, ccy, stamp_us);
    }

    // Copies cash moves with seq > since, oldest first. Returns the count.
    int cash_moves_since(int64_t since, ClrCashMove* out, int cap) const noexcept {
        if (!out || cap <= 0 || n_cash_ == 0) return 0;
        const int start = (cash_head_ - n_cash_ + kMaxCash) % kMaxCash;
        int got = 0;
        for (int i = 0; i < n_cash_ && got < cap; ++i) {
            const ClrCashMove& m = cash_[(start + i) % kMaxCash];
            if (m.seq > since) out[got++] = m;
        }
        return got;
    }

    int64_t cash_seq() const noexcept { return cash_seq_; }

    // ---- income, expense, shares -----------------------------------------

    // Books an accrued fee in whole currency units. A negative amount is a
    // reversal: it lowers the expense and returns the cash. The bucket only
    // splits reporting; the ledger keeps the signed total.
    void accrue_expense(uint8_t kind, int64_t amount, int64_t stamp_us) noexcept {
        if (amount == 0) return;
        expense_ += amount;
        switch (kind) {
            case kClrMgmtFee: mgmt_fee_ += amount; break;
            case kClrCustodyFee: custody_fee_ += amount; break;
            case kClrSalesFee: sales_fee_ += amount; break;
            default: break;
        }
        book_internal(kClrFee, amount, stamp_us);
    }

    // Financing interest follows the expense convention: positive is a cost,
    // negative is a reversal or a credit.
    void accrue_interest(int64_t amount, int64_t stamp_us) noexcept {
        if (amount == 0) return;
        expense_ += amount;
        fin_interest_ += amount;
        book_internal(kClrInterest, amount, stamp_us);
    }

    // Subscribe issues shares at the unit NAV before the movement and brings
    // the matching cash into the fund. Rejected when the book has no NAV.
    bool subscribe(int64_t shares, int64_t stamp_us) noexcept {
        if (shares <= 0 || shares_ <= 0) return false;
        const int64_t nav = unit_nav();
        if (nav <= 0) return false;
        const int64_t amount = shares * nav / kClrNavScale;
        if (amount <= 0) return false;
        shares_ += shares;
        shares_in_ += shares;
        subs_ += amount;
        flow_since_settle_ += amount;
        book_internal(kClrSubscribe, amount, stamp_us);
        return true;
    }

    // Redeem retires shares at the unit NAV before the movement. Redemption
    // above the share count is rejected without touching state.
    bool redeem(int64_t shares, int64_t stamp_us) noexcept {
        if (shares <= 0 || shares > shares_) return false;
        const int64_t nav = unit_nav();
        if (nav <= 0) return false;
        const int64_t amount = shares * nav / kClrNavScale;
        shares_ -= shares;
        shares_out_ += shares;
        redemptions_ += amount;
        flow_since_settle_ -= amount;
        book_internal(kClrRedeem, -amount, stamp_us);
        return true;
    }

    int64_t income() const noexcept { return income_; }
    int64_t expense() const noexcept { return expense_; }
    int64_t shares_in() const noexcept { return shares_in_; }
    int64_t shares_out() const noexcept { return shares_out_; }
    int64_t distributions_ppm() const noexcept { return distributions_ppm_; }

    // Cumulative NAV keeps distributions in the track record.
    int64_t cumulative_nav() const noexcept { return unit_nav() + distributions_ppm_; }

    // ---- corporate actions -----------------------------------------------

    // Applies a corporate action against the lots held now, which the caller
    // arranges after the previous settle and before the first fill of the
    // ex-date. One (name, event_id) pair is booked once; a replay is a no-op.
    ClrCorpOut apply_corp(const ClrCorp& c, int64_t now_us) noexcept {
        ClrCorpOut out{};
        if (c.name == 0) return out;
        for (int i = 0; i < n_corp_; ++i) {
            if (corp_[i].name == c.name && corp_[i].event_id == c.event_id) return out;
        }

        if (c.kind == kClrCashDiv) {
            const int64_t qty = net_qty(c.name);
            const int64_t gross = sat_mul(qty, c.per10_cash) / 10 / kClrPxScale;
            const int64_t tax = gross > 0 ? gross * c.tax_ppm / kClrPpm : 0;
            const int64_t net = gross - tax;
            out.cash = net;
            out.tax = tax;
            income_ += net;
            if (net != 0) book_internal(kClrDividend, net, now_us);
            if (gross > 0 && shares_ > 0) distributions_ppm_ += gross * kClrNavScale / shares_;
            out.applied = true;
            remember_corp(c);
            return out;
        }

        if (c.kind == kClrStockDiv) {
            const int64_t qty = net_qty(c.name);
            if (qty < 0) {
                out.short_blocked = true;
                return out;
            }
            const int64_t add = sat_mul(qty, c.per10_stock) / 10;
            if (add > 0) {
                int64_t mult = 1;
                int64_t margin = 0;
                uint8_t ccy = 0;
                bool today = true;
                bool found = false;
                for (int i = 0; i < n_lots_; ++i) {
                    const ClrLot& l = lots_[i];
                    if (l.name != c.name || l.side < 0 || l.qty <= 0) continue;
                    mult = l.mult;
                    margin = l.margin_ppm;
                    ccy = l.ccy;
                    today = l.today;
                    found = true;
                    break;
                }
                if (!found) return out;
                add_zero_cost(c.name, add, mult, margin, ccy, today);
                out.qty_added = add;
            }
            out.applied = true;
            remember_corp(c);
            return out;
        }

        if (c.kind == kClrSplit || c.kind == kClrMerge) {
            if (c.num <= 0 || c.den <= 0) return out;
            // A merge consolidates shares, so it must shrink quantity.
            if (c.kind == kClrMerge && c.num >= c.den) return out;
            if (net_qty(c.name) == 0) {
                out.applied = true;
                remember_corp(c);
                return out;
            }
            // Validate every lot before changing any of them.
            for (int i = 0; i < n_lots_; ++i) {
                const ClrLot& l = lots_[i];
                if (l.name != c.name || l.qty <= 0) continue;
                if (sat_mul(l.qty, c.num) / c.den <= 0) return out;
            }
            int64_t added = 0;
            int64_t removed = 0;
            for (int i = 0; i < n_lots_; ++i) {
                ClrLot& l = lots_[i];
                if (l.name != c.name || l.qty <= 0) continue;
                const int64_t nq = sat_mul(l.qty, c.num) / c.den;
                int64_t np = sat_mul(l.px, c.den) / c.num;
                int64_t ns = sat_mul(l.settle_px, c.den) / c.num;
                if (l.px > 0 && np <= 0) np = 1;
                if (l.settle_px > 0 && ns <= 0) ns = 1;
                if (nq > l.qty) added += nq - l.qty;
                else removed += l.qty - nq;
                l.qty = nq;
                l.px = np;
                l.settle_px = ns;
            }
            out.qty_added = added;
            out.qty_removed = removed;
            out.applied = true;
            remember_corp(c);
            return out;
        }
        return out;
    }

    // ---- fills -----------------------------------------------------------

    ClrFillOut apply(const ClrFillIn& f, int64_t stamp_us = 0) noexcept {
        ClrFillOut out{};
        if (f.qty <= 0 || f.px <= 0 || f.name == 0) return out;
        const int64_t mult = f.mult > 0 ? f.mult : 1;
        const int64_t notional = f.px * f.qty * mult / kClrPxScale;

        const int64_t close_fee_ppm = f.close_fee_ppm >= 0 ? f.close_fee_ppm : f.fee_ppm;
        const int64_t close_today_ppm = f.close_today_fee_ppm >= 0 ? f.close_today_fee_ppm : close_fee_ppm;

        int64_t remaining = f.qty;
        if (f.side == 1) {
            remaining = close_side(f.name, -1, remaining, f.px, mult, close_fee_ppm, close_today_ppm, out);
        } else {
            remaining = close_side(f.name, 1, remaining, f.px, mult, close_fee_ppm, close_today_ppm, out);
        }

        int64_t open_var = 0;
        const int8_t open_side = f.side == 1 ? 1 : -1;
        if (remaining > 0 && (f.side == 1 || f.allow_short)) {
            add_lot(f.name, open_side, remaining, f.px, mult, f.margin_ppm, f.ccy);
            out.opened_qty = remaining;
            open_var = remaining * f.px * mult / kClrPxScale * f.fee_ppm / kClrPpm;
        }
        out.open_fee = open_var;
        int64_t commission = out.open_fee + out.close_fee;
        if (f.min_fee > 0 && commission < f.min_fee) commission = f.min_fee;
        if (commission < 0) commission = 0;
        const int64_t stamp = f.side == 2 ? notional * f.stamp_ppm / kClrPpm : 0;
        out.commission = commission;
        out.stamp = stamp;
        out.fee = commission + stamp;

        commission_ += commission;
        stamp_ += stamp;
        ++trade_count_;
        turnover_ += notional;
        day_trades_ += 1;
        day_turnover_ += notional;

        NameAgg& a = agg(f.name, f.ccy);
        a.realized += out.realized;
        a.realized_settle += out.realized_settle;
        a.fees += out.fee;
        a.commission += commission;
        a.stamp += stamp;
        a.trades += 1;
        a.turnover += notional;
        day_realized_ += out.realized;
        day_fees_ += out.fee;
        add_trade(f, stamp_us, out);
        return out;
    }

    // Books the trade in the audit ring. The oldest entry is overwritten.
    void add_trade(const ClrFillIn& f, int64_t stamp_us, const ClrFillOut& out) noexcept {
        trade_ver_.fetch_add(1, std::memory_order_release);
        ClrTrade& t = trades_[trade_head_];
        t = ClrTrade{};
        t.seq = ++trade_seq_;
        t.stamp_us = stamp_us;
        t.name = f.name;
        t.side = f.side;
        t.qty = f.qty;
        t.px = f.px;
        t.fee = out.fee;
        t.realized = out.realized;
        t.realized_settle = out.realized_settle;
        t.closed_today = out.closed_today;
        t.closed_yday = out.closed_yday;
        trade_head_ = (trade_head_ + 1) % kMaxTrades;
        if (n_trades_ < kMaxTrades) ++n_trades_;
        trade_ver_.fetch_add(1, std::memory_order_release);
    }

    // Copies trades with seq > since, oldest first. Returns the count. A reader
    // on another thread retries when a write is in flight.
    int trades_since(int64_t since, ClrTrade* out, int cap) const noexcept {
        if (!out || cap <= 0) return 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const uint32_t v1 = trade_ver_.load(std::memory_order_acquire);
            if (v1 & 1u) continue;
            if (n_trades_ == 0) return 0;
            const int start = (trade_head_ - n_trades_ + kMaxTrades) % kMaxTrades;
            int got = 0;
            for (int i = 0; i < n_trades_ && got < cap; ++i) {
                const ClrTrade& t = trades_[(start + i) % kMaxTrades];
                if (t.seq > since) out[got++] = t;
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (trade_ver_.load(std::memory_order_acquire) == v1) return got;
        }
        return 0;
    }

    int64_t trade_seq() const noexcept { return trade_seq_; }

    // ---- day close -------------------------------------------------------

    // Closes the trading day. Positions, realised PnL, fees, and cash carry;
    // the daily counters restart and today's lots become yesterday's.
    void settle(int64_t now_us) noexcept { settle_impl(now_us, nullptr, 0); }

    // Same, but marks open lots to the settlement price first. The mark move is
    // booked as settled PnL and becomes the lot's new basis.
    void settle(int64_t now_us, const ClrMark* marks, int n) noexcept { settle_impl(now_us, marks, n); }

    int64_t realized() const noexcept {
        int64_t v = 0;
        for (int i = 0; i < n_names_; ++i) v += names_[i].realized;
        return v;
    }

    int64_t realized_settle() const noexcept {
        int64_t v = 0;
        for (int i = 0; i < n_names_; ++i) v += names_[i].realized_settle;
        return v;
    }

    int64_t fees() const noexcept {
        int64_t v = 0;
        for (int i = 0; i < n_names_; ++i) v += names_[i].fees;
        return v;
    }

    int64_t net_qty(uint32_t name) const noexcept {
        int64_t v = 0;
        for (int i = 0; i < n_lots_; ++i) {
            if (lots_[i].name == name) v += lots_[i].side * lots_[i].qty;
        }
        return v;
    }

    int64_t long_qty(uint32_t name) const noexcept { return side_qty(name, 1, -1); }
    int64_t short_qty(uint32_t name) const noexcept { return side_qty(name, -1, -1); }
    int64_t long_today_qty(uint32_t name) const noexcept { return side_qty(name, 1, 1); }
    int64_t long_yday_qty(uint32_t name) const noexcept { return side_qty(name, 1, 0); }
    int64_t short_today_qty(uint32_t name) const noexcept { return side_qty(name, -1, 1); }
    int64_t short_yday_qty(uint32_t name) const noexcept { return side_qty(name, -1, 0); }

    // ---- valuation -------------------------------------------------------

    // Close-basis unrealised PnL.
    bool unrealized(const uint32_t* marks_name, const int64_t* marks_px, int n, int64_t& out) const noexcept {
        ClrMark m[8]{};
        const int k = to_marks(marks_name, marks_px, n, m, 8);
        out = unrealized_of(m, k, false);
        return true;
    }

    bool unrealized(const ClrMark* marks, int n, int64_t& out) const noexcept {
        out = unrealized_of(marks, n, false);
        return true;
    }

    bool unrealized_settle(const ClrMark* marks, int n, int64_t& out) const noexcept {
        out = unrealized_of(marks, n, true);
        return true;
    }

    int64_t margin(const uint32_t* marks_name, const int64_t* marks_px, int n) const noexcept {
        ClrMark m[8]{};
        const int k = to_marks(marks_name, marks_px, n, m, 8);
        return margin_of(m, k);
    }

    int64_t margin(const ClrMark* marks, int n) const noexcept { return margin_of(marks, n); }

    // Fills rows, totals, and appends the latest marks for the CSV snapshot.
    void report(const uint32_t* marks_name, const int64_t* marks_px, int n, uint8_t env, ClearingReport& out) noexcept {
        ClrMark m[8]{};
        const int k = to_marks(marks_name, marks_px, n, m, 8);
        report(m, k, env, out);
    }

    void report(const ClrMark* marks, int n, uint8_t env, ClearingReport& out) noexcept {
        out = ClearingReport{};
        out.acct = acct_;
        out.env = env;
        env_ = env;
        const int rows = n_names_ < 8 ? n_names_ : 8;
        for (int i = 0; i < rows; ++i) {
            ClearingRow& r = out.row[i];
            const uint32_t name = names_[i].name;
            const int64_t fx = rate(names_[i].ccy);
            r.name = name;
            r.ccy = names_[i].ccy;
            r.fx_ppm = fx;
            r.realized = names_[i].realized;
            r.realized_settle = names_[i].realized_settle;
            r.fees = names_[i].fees;
            r.commission = names_[i].commission;
            r.stamp = names_[i].stamp;
            r.trades = names_[i].trades;
            r.turnover = names_[i].turnover;
            int64_t mark = 0;
            int64_t settle_mark = 0;
            for (int j = 0; j < n_lots_; ++j) {
                const ClrLot& l = lots_[j];
                if (l.name != name || l.qty <= 0) continue;
                mark = mark_of(name, marks, n, l.px, false);
                settle_mark = mark_of(name, marks, n, l.settle_px, true);
                if (l.side > 0) {
                    const int64_t before = r.long_qty;
                    r.long_qty += l.qty;
                    r.avg_long = (r.avg_long * before + l.px * l.qty) / (r.long_qty > 0 ? r.long_qty : 1);
                    if (l.today) r.long_today += l.qty;
                    else r.long_yday += l.qty;
                } else {
                    const int64_t before = r.short_qty;
                    r.short_qty += l.qty;
                    r.avg_short = (r.avg_short * before + l.px * l.qty) / (r.short_qty > 0 ? r.short_qty : 1);
                    if (l.today) r.short_today += l.qty;
                    else r.short_yday += l.qty;
                }
                const int64_t close_delta = (l.side > 0 ? (mark - l.px) : (l.px - mark)) * l.qty * l.mult / kClrPxScale;
                const int64_t settle_delta =
                    (l.side > 0 ? (settle_mark - l.settle_px) : (l.settle_px - settle_mark)) * l.qty * l.mult / kClrPxScale;
                r.unrealized += close_delta;
                r.unrealized_settle += settle_delta;
                r.margin += mark * l.qty * l.mult / kClrPxScale * l.margin_ppm / kClrPpm;
                r.market_value += mark * l.qty * l.mult / kClrPxScale;
            }
            r.mark = mark;
            r.settle_mark = settle_mark;
            last_mark_[i] = mark;
            last_settle_mark_[i] = settle_mark;
            if (fx <= 0) ++out.fx_missing;
            out.realized += conv(r.realized, fx);
            out.realized_settle += conv(r.realized_settle, fx);
            out.unrealized += conv(r.unrealized, fx);
            out.unrealized_settle += conv(r.unrealized_settle, fx);
            out.fees += conv(r.fees, fx);
            out.margin += conv(r.margin, fx);
            out.market_value += conv(r.market_value, fx);
            out.commission += conv(r.commission, fx);
            out.stamp += conv(r.stamp, fx);
            out.turnover += conv(r.turnover, fx);
        }
        out.rows = rows;
        out.stamp_us = 0;
        out.cash_in = cash_in_;
        out.deposits = deposits_;
        out.withdrawals = withdrawals_;
        out.cash_balance = cash_balance();
        out.cash_flow = out.realized - out.fees;
        out.equity = out.cash_balance + out.cash_flow + out.unrealized;
        out.available = out.equity - out.margin;
        out.equity_settle = out.cash_balance + out.realized_settle - out.fees + out.unrealized_settle;
        out.trades = trade_count_;
        out.day_realized = day_realized_;
        out.day_fees = day_fees_;
        out.day_trades = day_trades_;
        out.day_turnover = day_turnover_;
        out.day_settled = day_settled_;
        out.settle_us = settle_us_;
        out.day_seq = day_seq_;
        out.pre_equity = has_pre_ ? pre_equity_ : out.cash_balance;
        out.today_pnl = out.equity - out.pre_equity - flow_since_settle_;
        out.net_flow = deposits_ - withdrawals_ + subs_ - redemptions_;
        // Cumulative PnL is equity less the capital that funded it.
        out.total_pnl = out.equity - cash_in_ - out.net_flow;
        out.income = income_;
        out.expense = expense_;
        out.mgmt_fee = mgmt_fee_;
        out.custody_fee = custody_fee_;
        out.sales_fee = sales_fee_;
        out.fin_interest = fin_interest_;
        out.shares = shares_;
        out.shares_in = shares_in_;
        out.shares_out = shares_out_;
        out.unit_nav = shares_ > 0 ? out.equity * kClrNavScale / shares_ : 0;
        if (out.unit_nav > peak_nav_) peak_nav_ = out.unit_nav;
        out.peak_nav = peak_nav_;
        out.drawdown_ppm =
            peak_nav_ > 0 && out.unit_nav < peak_nav_ ? (peak_nav_ - out.unit_nav) * kClrPpm / peak_nav_ : 0;
        out.distributions_ppm = distributions_ppm_;
        out.cumulative_nav = out.unit_nav + distributions_ppm_;
        out.days = day_count_ < kClrMaxDays ? day_count_ : kClrMaxDays;
        for (int i = 0; i < out.days; ++i) {
            const int idx = (day_head_ - out.days + i + kClrMaxDays) % kClrMaxDays;
            out.day[i] = days_[idx];
        }
        out.moves = n_cash_ < 8 ? n_cash_ : 8;
        for (int i = 0; i < out.moves; ++i) {
            const int idx = (cash_head_ - out.moves + i + kMaxCash) % kMaxCash;
            out.move[i] = cash_[idx];
        }
        last_equity_ = out.equity;
        last_nav_ = out.unit_nav;
        has_nav_ = true;
    }

    // Compares cleared net quantity with PMS. Rows are appended to io.recon.
    template <int Cap>
    int reconcile(const Pms<Cap>& pms, uint32_t acct, ClearingReport& io) const noexcept {
        io.recon_rows = 0;
        io.recon_diff_rows = 0;
        for (int i = 0; i < n_names_ && io.recon_rows < 8; ++i) {
            const uint32_t name = names_[i].name;
            const int64_t clr = net_qty(name);
            const PmsPos* p = pms.find(acct, name);
            const int64_t pm = p ? (p->long_qty - p->short_qty) : 0;
            add_recon(io, name, clr, pm);
        }
        // PMS rows that never reached clearing still show up as differences.
        for (int i = 0; i < pms.size() && io.recon_rows < 8; ++i) {
            const PmsPos& p = pms.at(i);
            if (p.acct != acct) continue;
            if (find_agg(p.name) >= 0) continue;
            add_recon(io, p.name, 0, p.long_qty - p.short_qty);
        }
        return io.recon_rows;
    }

    bool snapshot_csv(const char* path, uint8_t mode) const noexcept {
        if (!path || !path[0]) return false;
        std::FILE* f = std::fopen(path, "w");
        if (!f) return false;
        std::fprintf(f,
                     "mode,acct,name,ccy,long,short,long_today,long_yday,short_today,short_yday,avg_long,avg_short,"
                     "realized,realized_settle,unrealized,unrealized_settle,fees,margin,mv,mark,settle_mark,"
                     "commission,stamp,trades,turnover,cum_nav,income,expense,net_flow\n");
        const int64_t cum_nav = cumulative_nav();
        const int64_t net_flow = deposits_ - withdrawals_ + subs_ - redemptions_;
        const int rows = n_names_ < 8 ? n_names_ : 8;
        for (int i = 0; i < rows; ++i) {
            const uint32_t name = names_[i].name;
            int64_t lq = 0, sq = 0, lqt = 0, lqy = 0, sqt = 0, sqy = 0, al = 0, as = 0;
            int64_t unreal = 0, unreal_s = 0, margin = 0;
            for (int j = 0; j < n_lots_; ++j) {
                const ClrLot& l = lots_[j];
                if (l.name != name || l.qty <= 0) continue;
                const int64_t mark = last_mark_[i] > 0 ? last_mark_[i] : l.px;
                const int64_t settle = last_settle_mark_[i] > 0 ? last_settle_mark_[i] : l.settle_px;
                if (l.side > 0) {
                    const int64_t before = lq;
                    lq += l.qty;
                    al = (al * before + l.px * l.qty) / (lq > 0 ? lq : 1);
                    if (l.today) lqt += l.qty;
                    else lqy += l.qty;
                } else {
                    const int64_t before = sq;
                    sq += l.qty;
                    as = (as * before + l.px * l.qty) / (sq > 0 ? sq : 1);
                    if (l.today) sqt += l.qty;
                    else sqy += l.qty;
                }
                unreal += (l.side > 0 ? (mark - l.px) : (l.px - mark)) * l.qty * l.mult / kClrPxScale;
                unreal_s += (l.side > 0 ? (settle - l.settle_px) : (l.settle_px - settle)) * l.qty * l.mult /
                            kClrPxScale;
                margin += mark * l.qty * l.mult / kClrPxScale * l.margin_ppm / kClrPpm;
            }
            const int64_t mark = last_mark_[i];
            const int64_t settle = last_settle_mark_[i];
            const int64_t mv = mark * (lq + sq) / kClrPxScale;
            std::fprintf(f,
                         "%u,%u,%u,%s,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
                         "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
                         static_cast<unsigned>(mode), static_cast<unsigned>(acct_), static_cast<unsigned>(name),
                         ccy_code(names_[i].ccy), static_cast<long long>(lq), static_cast<long long>(sq),
                         static_cast<long long>(lqt), static_cast<long long>(lqy), static_cast<long long>(sqt),
                         static_cast<long long>(sqy), static_cast<long long>(al), static_cast<long long>(as),
                         static_cast<long long>(names_[i].realized), static_cast<long long>(names_[i].realized_settle),
                         static_cast<long long>(unreal), static_cast<long long>(unreal_s),
                         static_cast<long long>(names_[i].fees), static_cast<long long>(margin),
                         static_cast<long long>(mv), static_cast<long long>(mark), static_cast<long long>(settle),
                         static_cast<long long>(names_[i].commission), static_cast<long long>(names_[i].stamp),
                         static_cast<long long>(names_[i].trades), static_cast<long long>(names_[i].turnover),
                         static_cast<long long>(cum_nav), static_cast<long long>(income_),
                         static_cast<long long>(expense_), static_cast<long long>(net_flow));
        }
        std::fclose(f);
        return true;
    }

    // ---- unit NAV --------------------------------------------------------

    void set_shares(int64_t shares) noexcept {
        shares_ = shares > 0 ? shares : 0;
        if (shares_ > 0 && peak_nav_ == 0 && last_equity_ > 0) {
            peak_nav_ = last_equity_ * kClrNavScale / shares_;
        }
    }

    int64_t shares() const noexcept { return shares_; }
    int64_t unit_nav() const noexcept {
        const int64_t eq = has_nav_ ? last_equity_ : cash_balance();
        return shares_ > 0 ? eq * kClrNavScale / shares_ : 0;
    }
    int64_t peak_nav() const noexcept { return peak_nav_; }
    int64_t drawdown_ppm() const noexcept {
        return peak_nav_ > 0 && unit_nav() < peak_nav_ ? (peak_nav_ - unit_nav()) * kClrPpm / peak_nav_ : 0;
    }
    int64_t day_count() const noexcept { return day_count_; }
    const ClrDayStat& day_at(int i) const noexcept { return days_[(day_head_ - day_count_ + i + kClrMaxDays) % kClrMaxDays]; }

private:
    struct NameAgg {
        uint32_t name{0};
        uint8_t ccy{0};
        int64_t realized{0};
        int64_t realized_settle{0};
        int64_t fees{0};
        int64_t commission{0};
        int64_t stamp{0};
        int64_t trades{0};
        int64_t turnover{0};
    };

    static constexpr int kMaxTrades = 64;
    static constexpr int kMaxCash = 64;

    NameAgg& agg(uint32_t name, uint8_t ccy) noexcept {
        int i = find_agg(name);
        if (i < 0) {
            if (n_names_ >= kMaxNames) return dummy_;
            names_[n_names_].name = name;
            names_[n_names_].ccy = ccy;
            i = n_names_++;
        }
        return names_[i];
    }

    int find_agg(uint32_t name) const noexcept {
        for (int i = 0; i < n_names_; ++i) {
            if (names_[i].name == name) return i;
        }
        return -1;
    }

    int64_t side_qty(uint32_t name, int8_t side, int today) const noexcept {
        int64_t v = 0;
        for (int i = 0; i < n_lots_; ++i) {
            const ClrLot& l = lots_[i];
            if (l.name != name || l.side != side) continue;
            if (today >= 0 && (today == 1) != l.today) continue;
            v += l.qty;
        }
        return v;
    }

    ClrCashMove book_cash(uint8_t kind, int64_t amount, uint8_t ccy, int64_t stamp_us) noexcept {
        ClrCashMove m{};
        if (amount <= 0 || kind < kClrDeposit || kind > kClrWithdraw) return m;
        const int64_t fx = rate(ccy);
        if (fx <= 0) return m;
        const int64_t base = amount * fx / kClrPpm;
        if (base <= 0) return m;
        if (kind == kClrWithdraw && base > cash_balance()) return m;
        m.seq = ++cash_seq_;
        m.stamp_us = stamp_us;
        m.kind = kind;
        m.ccy = ccy;
        m.amount = amount;
        m.base_amount = base;
        if (kind == kClrDeposit) {
            deposits_ += base;
            flow_since_settle_ += base;
        } else {
            withdrawals_ += base;
            flow_since_settle_ -= base;
        }
        cash_[cash_head_] = m;
        cash_head_ = (cash_head_ + 1) % kMaxCash;
        if (n_cash_ < kMaxCash) ++n_cash_;
        return m;
    }

    // Dividend, fee, interest, and share movements are booked in base currency
    // and never move the deposit/withdraw counters.
    void book_internal(uint8_t kind, int64_t amount, int64_t stamp_us) noexcept {
        ClrCashMove m{};
        m.seq = ++cash_seq_;
        m.stamp_us = stamp_us;
        m.kind = kind;
        m.ccy = 0;
        m.amount = amount;
        m.base_amount = amount;
        cash_[cash_head_] = m;
        cash_head_ = (cash_head_ + 1) % kMaxCash;
        if (n_cash_ < kMaxCash) ++n_cash_;
    }

    void add_zero_cost(uint32_t name, int64_t qty, int64_t mult, int64_t margin, uint8_t ccy,
                       bool today) noexcept {
        if (qty <= 0) return;
        if (n_lots_ < kMaxLots) {
            ClrLot& l = lots_[n_lots_++];
            l = ClrLot{};
            l.name = name;
            l.side = 1;
            l.qty = qty;
            l.px = 0;
            l.settle_px = 0;
            l.mult = mult;
            l.margin_ppm = margin;
            l.ccy = ccy;
            l.today = today;
            l.open_id = next_open_id_++;
            return;
        }
        for (int i = n_lots_ - 1; i >= 0; --i) {
            ClrLot& l = lots_[i];
            if (l.name == name && l.side > 0) {
                l.qty += qty;
                return;
            }
        }
    }

    void remember_corp(const ClrCorp& c) noexcept {
        corp_[corp_head_] = CorpSeen{c.name, c.event_id};
        corp_head_ = (corp_head_ + 1) % kCorpSeen;
        if (n_corp_ < kCorpSeen) ++n_corp_;
    }

    // Saturating 64-bit multiply: split ratios and bonus quantities never wrap.
    static int64_t sat_mul(int64_t a, int64_t b) noexcept {
        if (a == 0 || b == 0) return 0;
        const bool neg = (a < 0) != (b < 0);
        const uint64_t ua = a < 0 ? static_cast<uint64_t>(-(a + 1)) + 1u : static_cast<uint64_t>(a);
        const uint64_t ub = b < 0 ? static_cast<uint64_t>(-(b + 1)) + 1u : static_cast<uint64_t>(b);
        if (ub != 0 && ua > static_cast<uint64_t>(INT64_MAX) / ub) {
            return neg ? INT64_MIN : INT64_MAX;
        }
        return a * b;
    }

    void settle_impl(int64_t now_us, const ClrMark* marks, int n) noexcept {
        int64_t settled = 0;
        if (marks && n > 0) {
            for (int i = 0; i < n_lots_; ++i) {
                ClrLot& l = lots_[i];
                if (l.qty <= 0) continue;
                const int64_t s = mark_of(l.name, marks, n, l.settle_px, true);
                if (s <= 0) continue;
                const int64_t delta =
                    (l.side > 0 ? (s - l.settle_px) : (l.settle_px - s)) * l.qty * l.mult / kClrPxScale;
                if (delta != 0) {
                    agg(l.name, l.ccy).realized_settle += delta;
                    settled += delta;
                }
                l.settle_px = s;
            }
            for (int i = 0; i < n; ++i) {
                const int k = find_agg(marks[i].name);
                if (k < 0) continue;
                const int64_t s = marks[i].settle > 0 ? marks[i].settle : marks[i].last;
                if (s > 0) last_settle_mark_[k] = s;
            }
        }
        day_settled_ = settled;

        ClrDayStat& d = days_[day_head_];
        d = ClrDayStat{};
        d.day_seq = day_seq_ + 1;
        d.settle_us = now_us;
        const int64_t equity = has_nav_ ? last_equity_ : cash_balance();
        d.equity = equity;
        d.cash_balance = cash_balance();
        d.unit_nav = last_nav_;
        d.realized = day_realized_;
        d.settled = settled;
        d.fees = day_fees_;
        d.trades = day_trades_;
        d.turnover = day_turnover_;
        d.deposits = deposits_;
        d.withdrawals = withdrawals_;
        d.income = income_;
        d.expense = expense_;
        d.cum_nav = d.unit_nav + distributions_ppm_;
        // PnL of the day net of capital that arrived or left.
        d.pnl = equity - (has_pre_ ? pre_equity_ : cash_balance()) - flow_since_settle_;
        day_head_ = (day_head_ + 1) % kClrMaxDays;
        if (day_count_ < kClrMaxDays) ++day_count_;

        pre_equity_ = d.equity;
        has_pre_ = true;
        flow_since_settle_ = 0;
        settle_us_ = now_us;
        ++day_seq_;
        day_realized_ = 0;
        day_fees_ = 0;
        day_trades_ = 0;
        day_turnover_ = 0;
        for (int i = 0; i < n_lots_; ++i) lots_[i].today = false;
    }

    void add_lot(uint32_t name, int8_t side, int64_t qty, int64_t px, int64_t mult, int64_t margin_ppm,
                 uint8_t ccy) noexcept {
        if (n_lots_ >= kMaxLots) {
            // Capacity exhausted: fold into the newest matching lot so the
            // position still reconciles even if only the average price drifts.
            for (int i = n_lots_ - 1; i >= 0; --i) {
                ClrLot& l = lots_[i];
                if (l.name == name && l.side == side) {
                    const int64_t total = l.qty + qty;
                    l.px = (l.px * l.qty + px * qty) / (total > 0 ? total : 1);
                    l.settle_px = (l.settle_px * l.qty + px * qty) / (total > 0 ? total : 1);
                    l.qty = total;
                    return;
                }
            }
            return;
        }
        ClrLot& l = lots_[n_lots_++];
        l = ClrLot{};
        l.name = name;
        l.side = side;
        l.qty = qty;
        l.px = px;
        l.settle_px = px;
        l.mult = mult;
        l.margin_ppm = margin_ppm;
        l.ccy = ccy;
        l.today = true;
        l.open_id = next_open_id_++;
    }

    int64_t close_side(uint32_t name, int8_t side, int64_t qty, int64_t px, int64_t mult, int64_t close_fee_ppm,
                       int64_t close_today_ppm, ClrFillOut& out) noexcept {
        int64_t remaining = qty;
        for (int i = 0; i < n_lots_ && remaining > 0;) {
            ClrLot& l = lots_[i];
            if (l.name != name || l.side != side || l.qty <= 0) {
                ++i;
                continue;
            }
            const int64_t take = l.qty < remaining ? l.qty : remaining;
            const int64_t close_delta = (side > 0 ? (px - l.px) : (l.px - px)) * take * l.mult / kClrPxScale;
            const int64_t settle_delta =
                (side > 0 ? (px - l.settle_px) : (l.settle_px - px)) * take * l.mult / kClrPxScale;
            out.realized += close_delta;
            out.realized_settle += settle_delta;
            out.closed_qty += take;
            if (l.today) out.closed_today += take;
            else out.closed_yday += take;
            const int64_t rate = l.today ? close_today_ppm : close_fee_ppm;
            out.close_fee += take * px * l.mult / kClrPxScale * rate / kClrPpm;
            remaining -= take;
            l.qty -= take;
            if (l.qty == 0) {
                for (int j = i; j + 1 < n_lots_; ++j) lots_[j] = lots_[j + 1];
                --n_lots_;
                lots_[n_lots_] = ClrLot{};
            } else {
                ++i;
            }
        }
        (void)mult;
        return remaining;
    }

    static int64_t mark_of(uint32_t name, const ClrMark* marks, int n, int64_t fallback, bool settle) noexcept {
        if (marks) {
            for (int i = 0; i < n; ++i) {
                if (marks[i].name != name) continue;
                const int64_t v = settle ? (marks[i].settle > 0 ? marks[i].settle : marks[i].last) : marks[i].last;
                return v > 0 ? v : fallback;
            }
        }
        return fallback;
    }

    int64_t unrealized_of(const ClrMark* marks, int n, bool settle) const noexcept {
        int64_t total = 0;
        for (int i = 0; i < n_lots_; ++i) {
            const ClrLot& l = lots_[i];
            if (l.qty <= 0) continue;
            if (settle) {
                const int64_t mark = mark_of(l.name, marks, n, l.settle_px, true);
                total += (l.side > 0 ? (mark - l.settle_px) : (l.settle_px - mark)) * l.qty * l.mult / kClrPxScale;
            } else {
                const int64_t mark = mark_of(l.name, marks, n, l.px, false);
                total += (l.side > 0 ? (mark - l.px) : (l.px - mark)) * l.qty * l.mult / kClrPxScale;
            }
        }
        return total;
    }

    int64_t margin_of(const ClrMark* marks, int n) const noexcept {
        int64_t total = 0;
        for (int i = 0; i < n_lots_; ++i) {
            const ClrLot& l = lots_[i];
            if (l.qty <= 0 || l.margin_ppm <= 0) continue;
            const int64_t mark = mark_of(l.name, marks, n, l.px, false);
            const int64_t notional = mark * l.qty * l.mult / kClrPxScale;
            total += notional * l.margin_ppm / kClrPpm;
        }
        return total;
    }

    static int to_marks(const uint32_t* names, const int64_t* px, int n, ClrMark* out, int cap) noexcept {
        if (!names || !px || !out || cap <= 0) return 0;
        const int k = n < cap ? n : cap;
        for (int i = 0; i < k; ++i) {
            out[i].name = names[i];
            out[i].last = px[i];
            out[i].settle = px[i];
        }
        return k;
    }

    static int64_t conv(int64_t v, int64_t fx) noexcept { return fx > 0 ? v * fx / kClrPpm : 0; }

    static void add_recon(ClearingReport& io, uint32_t name, int64_t clr, int64_t pms_qty) noexcept {
        ClearingRecon& r = io.recon[io.recon_rows++];
        r.name = name;
        r.clr_qty = clr;
        r.pms_qty = pms_qty;
        r.diff = clr - pms_qty;
        if (r.diff != 0) ++io.recon_diff_rows;
    }

    struct CorpSeen {
        uint32_t name{0};
        int64_t event_id{0};
    };
    static constexpr int kCorpSeen = 64;

    ClrLot lots_[kMaxLots]{};
    NameAgg names_[kMaxNames]{};
    ClrCcy ccy_[kClrMaxCcy]{};
    int64_t last_mark_[kMaxNames]{};
    int64_t last_settle_mark_[kMaxNames]{};
    int n_lots_{0};
    int n_names_{0};
    int32_t next_open_id_{1};
    uint32_t acct_{0};
    uint8_t env_{0};
    NameAgg dummy_{};

    ClrTrade trades_[kMaxTrades]{};
    mutable std::atomic<uint32_t> trade_ver_{0};
    int n_trades_{0};
    int trade_head_{0};
    int64_t trade_seq_{0};

    ClrCashMove cash_[kMaxCash]{};
    int n_cash_{0};
    int cash_head_{0};
    int64_t cash_seq_{0};
    int64_t deposits_{0};
    int64_t withdrawals_{0};
    int64_t flow_since_settle_{0};

    ClrDayStat days_[kClrMaxDays]{};
    int day_head_{0};
    int day_count_{0};

    int64_t cash_in_{0};
    int64_t commission_{0};
    int64_t stamp_{0};
    int64_t trade_count_{0};
    int64_t turnover_{0};
    int64_t day_realized_{0};
    int64_t day_fees_{0};
    int64_t day_trades_{0};
    int64_t day_turnover_{0};
    int64_t day_settled_{0};
    int64_t settle_us_{0};
    uint32_t day_seq_{0};
    int64_t pre_equity_{0};
    bool has_pre_{false};
    int64_t shares_{0};
    int64_t peak_nav_{0};
    int64_t last_equity_{0};
    int64_t last_nav_{0};
    bool has_nav_{false};
    int64_t income_{0};
    int64_t expense_{0};
    int64_t mgmt_fee_{0};
    int64_t custody_fee_{0};
    int64_t sales_fee_{0};
    int64_t fin_interest_{0};
    int64_t shares_in_{0};
    int64_t shares_out_{0};
    int64_t subs_{0};
    int64_t redemptions_{0};
    int64_t distributions_ppm_{0};
    CorpSeen corp_[kCorpSeen]{};
    int n_corp_{0};
    int corp_head_{0};
};

// Calendar year of a microsecond stamp, UTC. Used for the year-to-date sum
// without pulling in <ctime> on the hot path.
inline int clr_year_of(int64_t us) noexcept {
    int64_t z = us / 86400000000ll;
    if (us < 0 && us % 86400000000ll != 0) --z;
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const int64_t year = y + (mp >= 10 ? 1 : 0);
    return static_cast<int>(year);
}

// Period sums over the report snapshot: the current day plus the last six and
// last twenty-nine settled days, and every record in the current year.
inline ClrPeriod clr_period(const ClearingReport& rep, int64_t now_us) noexcept {
    ClrPeriod p{};
    p.today = rep.today_pnl;
    p.d7 = p.today;
    p.d30 = p.today;
    p.ytd = p.today;
    const int year = clr_year_of(now_us);
    for (int i = rep.days - 1, k = 1; i >= 0; --i, ++k) {
        const ClrDayStat& d = rep.day[i];
        if (k < 7) p.d7 += d.pnl;
        if (k < 30) p.d30 += d.pnl;
        if (clr_year_of(d.settle_us) == year) p.ytd += d.pnl;
    }
    return p;
}

// Double-buffered snapshot for the web console.
class ClearingBoard {
public:
    void publish(const ClearingReport& r) noexcept {
        const uint32_t slot = idx_.load(std::memory_order_relaxed);
        seq_.fetch_add(1, std::memory_order_acq_rel);
        buf_[slot] = r;
        idx_.store(slot ^ 1u, std::memory_order_release);
        seq_.fetch_add(1, std::memory_order_release);
    }

    bool read(ClearingReport& out) const noexcept {
        const uint64_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1u) return false;
        const uint32_t slot = idx_.load(std::memory_order_acquire) ^ 1u;
        out = buf_[slot];
        std::atomic_thread_fence(std::memory_order_acquire);
        return seq_.load(std::memory_order_acquire) == s1;
    }

private:
    ClearingReport buf_[2]{};
    std::atomic<uint32_t> idx_{0};
    std::atomic<uint64_t> seq_{0};
};

}  // namespace pitlane
