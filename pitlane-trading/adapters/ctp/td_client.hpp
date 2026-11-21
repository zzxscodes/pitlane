#pragma once

// CTP trader adapter. Desk TdClient / TdEngine stay broker-neutral.

#include "config.hpp"
#include "pitlane/desk/instmap.hpp"
#include "pitlane/desk/session.hpp"
#include "pitlane/desk/td_engine.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/order/ledger_view.hpp"
#include "pitlane/time/cycle.hpp"
#include "pitlane/util/hint.hpp"

#include "ThostFtdcTraderApi.h"

#include <atomic>
#include <cstdint>

namespace pitlane {

class WireLog;

struct LiveOrder {
    uint64_t tag{0};
    uint64_t t0_cycles{0};
    int64_t t0_us{0};
    uint64_t local_ns{0};
    uint64_t gate_ns{0};
    uint32_t name{0};
    uint8_t cause{0};  // force-close reason from the Intent
    char instrument[32]{};
    char exchange[9]{};
    char sys_id[21]{};
};

class CtpTdClient : public TdClient, public CThostFtdcTraderSpi {
public:
    static constexpr int kLive = 64;
    // Terminal order reports can arrive before their trade reports. Keep a
    // short ring of ref -> tag so a late trade still finds its child.
    static constexpr int kRecent = 256;
    using AcceptFn = void (*)(void* user, const LiveOrder& lo, int64_t now_us, uint64_t roundtrip_ns);
    using TradeFn = void (*)(void* user, uint64_t tag, uint32_t name, int64_t px, int64_t qty, int64_t now_us);
    using DoneFn = void (*)(void* user, uint64_t tag, int64_t filled, int status, int64_t now_us);
    using RejectFn = void (*)(void* user, uint64_t tag, int err_id, int64_t now_us);
    // Full counter view. kind 1 replaces assets, kind 2 replaces positions.
    // Rows are flat snapshots from one query and arrive on the CTP thread.
    using LedgerFn = void (*)(void* user, int kind, const LedgerAsset* assets, int n_assets,
                              const LedgerPosition* positions, int n_positions, int64_t stamp_us);

    explicit CtpTdClient(Account acct, trading::CtpCfg cfg);
    ~CtpTdClient() override;

    bool start();
    void stop();

    int insert(const Intent& in) override;
    int cancel(uint64_t tag, uint32_t name) override;
    int query_asset(const QueryNote& note) override;
    int query_position(const QueryNote& note) override;
    int exercise(const ExecNote& note) override;
    bool ready() const noexcept override { return ready_.load(std::memory_order_acquire); }
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

    void bind_inst(const InstMap& map) noexcept;
    void set_scale(const CycleScale* scale) noexcept { scale_ = scale; }
    void set_log(WireLog* log) noexcept { log_ = log; }
    void set_accept(AcceptFn fn, void* user) noexcept {
        accept_fn_ = fn;
        accept_user_ = user;
    }
    void set_trade(TradeFn fn, void* user) noexcept {
        trade_fn_ = fn;
        trade_user_ = user;
    }
    void set_done(DoneFn fn, void* user) noexcept {
        done_fn_ = fn;
        done_user_ = user;
    }
    void set_reject(RejectFn fn, void* user) noexcept {
        reject_fn_ = fn;
        reject_user_ = user;
    }
    void set_report_sink(TdEngine* eng) noexcept { eng_ = eng; }
    void set_ledger(LedgerFn fn, void* user) noexcept {
        ledger_fn_ = fn;
        ledger_user_ = user;
    }
    void set_ledger_acct(uint32_t acct) noexcept { ledger_acct_ = acct; }
    // Contract multipliers let the ledger turn the counter's open and position
    // cost into average prices; without them those two fields stay zero.
    using MultFn = int64_t (*)(void* user, uint32_t name);
    void set_instrument_mult(MultFn fn, void* user) noexcept {
        mult_fn_ = fn;
        mult_user_ = user;
    }

    // Issues an account and a position query for the console ledger view.
    // Cheap enough to run every few seconds; replies replace the snapshot.
    void query_ledger() noexcept;

    PITLANE_HOT void note_t0(uint64_t tag, uint64_t t0_cycles, int64_t t0_us, uint64_t local_ns) noexcept;

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField, CThostFtdcRspInfoField* pRspInfo,
                           int nRequestID, bool bIsLast) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                        bool bIsLast) override;
    void OnRspUserPasswordUpdate(CThostFtdcUserPasswordUpdateField* pUserPasswordUpdate, CThostFtdcRspInfoField* pRspInfo,
                                 int nRequestID, bool bIsLast) override;
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                    CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;
    void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                          bool bIsLast) override;
    void OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo) override;
    void OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction, CThostFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) override;
    void OnErrRtnOrderAction(CThostFtdcOrderActionField* pOrderAction, CThostFtdcRspInfoField* pRspInfo) override;
    void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
    void OnRtnTrade(CThostFtdcTradeField* pTrade) override;
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount, CThostFtdcRspInfoField* pRspInfo,
                                int nRequestID, bool bIsLast) override;
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition, CThostFtdcRspInfoField* pRspInfo,
                                  int nRequestID, bool bIsLast) override;

private:
    bool ok_rsp(CThostFtdcRspInfoField* info, const char* what);
    void finish_ref(int ref) noexcept;
    void login() noexcept;
    void rotate_password() noexcept;
    int find_live(int ref) const noexcept;
    int find_t0(uint64_t tag) const noexcept;
    bool find_tag(int ref, uint64_t& tag, uint32_t& name) const noexcept;

    // Account and position replies arrive as many rows plus a final flag, so a
    // query stays open until bIsLast and accumulates into `sum`.
    struct PendQry {
        int id{0};
        uint8_t kind{0};
        uint32_t acct{0};
        uint64_t req{0};
        int64_t sum{0};
    };
    static constexpr int kQry = 16;
    int qry_find(int id) const noexcept;
    int qry_open(uint8_t kind, const QueryNote& note) noexcept;
    void qry_reply(int id, int64_t value) noexcept;
    uint32_t name_of(const char* code) const noexcept;

    struct LedgerAcc {
        LedgerAsset assets[8]{};
        LedgerPosition positions[kLive]{};
        double cost[kLive]{};
        double open_cost[kLive]{};
        int asset_n{0};
        int position_n{0};
        int64_t stamp_us{0};
    };
    void add_ledger_asset(const CThostFtdcTradingAccountField& row, int64_t stamp_us) noexcept;
    void add_ledger_position(const CThostFtdcInvestorPositionField& row, int64_t stamp_us) noexcept;
    void emit_ledger(int kind, uint64_t req) noexcept;

    Account acct_;
    trading::CtpCfg cfg_;
    CThostFtdcTraderApi* api_{nullptr};
    TdEngine* eng_{nullptr};
    WireLog* log_{nullptr};
    const CycleScale* scale_{nullptr};
    AcceptFn accept_fn_{nullptr};
    void* accept_user_{nullptr};
    TradeFn trade_fn_{nullptr};
    void* trade_user_{nullptr};
    DoneFn done_fn_{nullptr};
    void* done_user_{nullptr};
    RejectFn reject_fn_{nullptr};
    void* reject_user_{nullptr};
    int req_{0};
    int front_id_{0};
    int session_id_{0};
    std::atomic<int> max_ref_{0};
    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    uint8_t pw_tried_{0};

    char name_code_[InstMap::kMax][32]{};
    uint32_t name_id_[InstMap::kMax]{};
    int name_n_{0};

    LiveOrder t0_slot_[kLive]{};
    uint8_t t0_used_[kLive]{};
    LiveOrder live_slot_[kLive]{};
    int live_ref_[kLive]{};
    uint8_t live_used_[kLive]{};

    struct RecentRef {
        int ref;
        uint64_t tag;
        uint32_t name;
    };
    RecentRef recent_[kRecent]{};
    uint32_t recent_pos_{0};

    PendQry qry_[kQry]{};
    LedgerFn ledger_fn_{nullptr};
    void* ledger_user_{nullptr};
    uint32_t ledger_acct_{1};
    bool ledger_asset_turn_{true}; // the counter allows one query per second
    MultFn mult_fn_{nullptr};
    void* mult_user_{nullptr};
    LedgerAcc ledger_assets_{};
    LedgerAcc ledger_positions_{};
};

} // namespace pitlane
