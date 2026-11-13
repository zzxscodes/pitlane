#pragma once

// CTP market-data adapter. The engine roles live in desk/; this file only talks to Thost.

#include "pitlane/desk/instmap.hpp"
#include "pitlane/desk/md_engine.hpp"
#include "pitlane/desk/session.hpp"

#include "ThostFtdcMdApi.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace pitlane {

class WireLog;

class CtpMdEngine : public MdEngine, public CThostFtdcMdSpi {
public:
    using TickFn = void (*)(void* user, uint32_t name, const Quote& q, uint64_t t0_cycles);

    explicit CtpMdEngine(Account acct);
    ~CtpMdEngine();

    bool set_up() override;
    void pre_run() override;
    void post_run() override;
    bool subscribe(const char* const* codes, int n) override;

    void set_tick_fn(TickFn fn, void* user) noexcept {
        tick_fn_ = fn;
        tick_user_ = user;
    }
    void set_log(WireLog* log) noexcept { log_ = log; }
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    const InstMap& inst() const noexcept { return inst_; }

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                        bool bIsLast) override;
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument, CThostFtdcRspInfoField* pRspInfo,
                            int nRequestID, bool bIsLast) override;
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pData) override;

    bool logged_in() const noexcept { return logged_in_.load(std::memory_order_acquire); }

private:
    Account acct_;
    CThostFtdcMdApi* api_{nullptr};
    WireLog* log_{nullptr};
    int req_{0};
    std::atomic<bool> logged_in_{false};
    std::atomic<bool> failed_{false};
    InstMap inst_{};
    std::vector<std::string> pending_sub_;
    TickFn tick_fn_{nullptr};
    void* tick_user_{nullptr};
};

} // namespace pitlane
