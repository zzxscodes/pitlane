#pragma once

// CTP trader adapter. Desk TdClient / TdEngine stay broker-neutral.

#include "pitlane/desk/instmap.hpp"
#include "pitlane/desk/session.hpp"
#include "pitlane/desk/td_engine.hpp"
#include "pitlane/desk/types.hpp"
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
    char instrument[32]{};
};

class CtpTdClient : public TdClient, public CThostFtdcTraderSpi {
public:
    static constexpr int kLive = 64;
    using AcceptFn = void (*)(void* user, const LiveOrder& lo, int64_t now_us, uint64_t roundtrip_ns);

    explicit CtpTdClient(Account acct);
    ~CtpTdClient() override;

    bool start();
    void stop();

    int insert(const Intent& in) override;
    int cancel(uint64_t tag, uint32_t name) override;
    bool ready() const noexcept override { return ready_.load(std::memory_order_acquire); }
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

    void bind_inst(const InstMap& map) noexcept;
    void set_scale(const CycleScale* scale) noexcept { scale_ = scale; }
    void set_log(WireLog* log) noexcept { log_ = log; }
    void set_accept(AcceptFn fn, void* user) noexcept {
        accept_fn_ = fn;
        accept_user_ = user;
    }
    void set_report_sink(TdEngine* eng) noexcept { eng_ = eng; }

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
    void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
    void OnRtnTrade(CThostFtdcTradeField* pTrade) override;

private:
    bool ok_rsp(CThostFtdcRspInfoField* info, const char* what);
    void finish_ref(int ref) noexcept;
    void login() noexcept;
    void rotate_password() noexcept;
    int find_live(int ref) const noexcept;
    int find_t0(uint64_t tag) const noexcept;

    Account acct_;
    CThostFtdcTraderApi* api_{nullptr};
    TdEngine* eng_{nullptr};
    WireLog* log_{nullptr};
    const CycleScale* scale_{nullptr};
    AcceptFn accept_fn_{nullptr};
    void* accept_user_{nullptr};
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
};

} // namespace pitlane
