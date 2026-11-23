#pragma once

// CTP market-data adapter. The engine roles live in desk/; this file only talks to Thost.
//
// Every depth frame runs the same four steps in order:
//   raw fields -> ctp_decode -> CtpDepthBook::apply -> CtpPrintMaker::on_snap -> fanout
// The decoded snapshot and the reconstructed prints are published to the console
// through MarketBoard, and each step is timed into HopLedger.

#include "book.hpp"
#include "config.hpp"
#include "decode.hpp"
#include "fanout.hpp"
#include "../market_view.hpp"
#include "pitlane/desk/instmap.hpp"
#include "pitlane/desk/md_engine.hpp"
#include "pitlane/desk/session.hpp"
#include "pitlane/time/cycle.hpp"
#include "pitlane/time/hops.hpp"

#include "ThostFtdcMdApi.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace pitlane {

class WireLog;

class CtpMdEngine : public MdEngine, public CThostFtdcMdSpi {
public:
    using TickFn = void (*)(void* user, uint32_t name, const Quote& q, uint64_t t0_cycles);

    explicit CtpMdEngine(Account acct, trading::CtpCfg cfg);
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

    // Read-only views for the ops console. The board is written on the market-data
    // thread; readers copy the whole snapshot under its seqlock.
    const MarketBoard& live() const noexcept { return live_; }
    const HopLedger& hops() const noexcept { return hops_; }
    const CtpBookSet& books() const noexcept { return books_; }
    const CtpFanout& fanout() const noexcept { return fan_; }

    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo, int nRequestID,
                        bool bIsLast) override;
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument, CThostFtdcRspInfoField* pRspInfo,
                            int nRequestID, bool bIsLast) override;
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pData) override;

    bool logged_in() const noexcept { return logged_in_.load(std::memory_order_acquire); }

private:
    // Shares the order-book set capacity so every catalogued instrument keeps its
    // own print maker and last book event.
    static constexpr int kMaxNames = CtpBookSet::kNames;
    static constexpr int kPrintRing = 64;
    static constexpr int kPrintsPerSnap = 4;

    static bool live_snap(void* user, const CtpSnap& s) noexcept;
    static bool live_print(void* user, const CtpPrint& p) noexcept;
    void mark_hot(uint32_t name) noexcept;
    void push_print(const CtpPrint& p) noexcept;
    void publish_live(int64_t stamp_us) noexcept;
    uint64_t ns_of(uint64_t begin, uint64_t end) const noexcept {
        uint64_t ns = 0;
        (void)scale_span(scale_, begin, end, ns);
        return ns;
    }

    Account acct_;
    trading::CtpCfg cfg_;
    CThostFtdcMdApi* api_{nullptr};
    WireLog* log_{nullptr};
    int req_{0};
    std::atomic<bool> logged_in_{false};
    std::atomic<bool> failed_{false};
    InstMap inst_{};
    std::vector<std::string> pending_sub_;
    TickFn tick_fn_{nullptr};
    void* tick_user_{nullptr};

    CtpBookSet books_{};
    CtpPrintMaker prints_[kMaxNames]{};
    CtpFanout fan_{};
    MarketBoard live_{};
    HopLedger hops_{};
    CycleScale scale_{};

    uint32_t hot_[kMarketBooks]{};
    int hot_n_{0};
    uint8_t last_ev_[kMaxNames]{};
    CtpPrint ring_[kPrintRing]{};
    uint32_t ring_head_{0};
    uint32_t ring_n_{0};
    uint64_t decoded_{0};
    uint64_t decode_err_{0};
    uint64_t print_total_{0};
    bool scale_ok_{false};
};

} // namespace pitlane
