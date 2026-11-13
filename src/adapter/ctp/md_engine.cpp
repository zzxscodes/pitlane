#include "pitlane/adapter/ctp/md_engine.hpp"

#include "pitlane/time/cycle.hpp"
#include "pitlane/util/textutil.hpp"
#include "pitlane/util/wirelog.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace pitlane {
namespace {

void say(WireLog* log, int level, const char* text) noexcept {
    if (log) log->log(level, lit(text));
}

void say_i(WireLog* log, int level, const char* text, int v) noexcept {
    if (log) log->log(level, lit(text), v);
}

void say_err(WireLog* log, const char* text, int id, const char* gbk) noexcept {
    char utf[320];
    utf[0] = 0;
    if (gbk && gbk[0] && !gbk_to_utf8(gbk, std::strlen(gbk), utf, sizeof utf)) std::snprintf(utf, sizeof utf, "%s", gbk);
    if (log) log->log(0, lit(text), id, lit(" "), utf);
}

bool bad_px(double x) noexcept { return x > 1e15 || x < -1e15; }

int64_t hhmmss_ms_to_us(const char* hhmmss, int ms) noexcept {
    if (!hhmmss || std::strlen(hhmmss) < 8) return 0;
    const int h = (hhmmss[0] - '0') * 10 + (hhmmss[1] - '0');
    const int m = (hhmmss[3] - '0') * 10 + (hhmmss[4] - '0');
    const int s = (hhmmss[6] - '0') * 10 + (hhmmss[7] - '0');
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return 0;
    return (static_cast<int64_t>(h) * 3600 + m * 60 + s) * 1000000ll + static_cast<int64_t>(ms) * 1000ll;
}

int64_t px_x10000(double px) noexcept {
    if (px <= 0) return 0;
    return static_cast<int64_t>(px * 10000.0 + 0.5);
}

int64_t now_us() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000ll + ts.tv_nsec / 1000;
}

} // namespace

CtpMdEngine::CtpMdEngine(Account acct) : acct_(std::move(acct)) {}

CtpMdEngine::~CtpMdEngine() { post_run(); }

bool CtpMdEngine::set_up() {
    if (!MdEngine::set_up()) return false;
    pending_sub_ = acct_.instruments;
    inst_ = InstMap{};
    for (std::size_t i = 0; i < pending_sub_.size(); ++i) {
        if (!inst_.add(pending_sub_[i].c_str(), static_cast<uint32_t>(i + 1))) return false;
    }
    return !pending_sub_.empty();
}

void CtpMdEngine::pre_run() {
    api_ = CThostFtdcMdApi::CreateFtdcMdApi(acct_.flow_md_dir.c_str());
    if (!api_) {
        say(log_, 0, "[md] CreateFtdcMdApi failed");
        request_stop();
        return;
    }
    api_->RegisterSpi(this);
    char front[160];
    std::snprintf(front, sizeof front, "%s", acct_.md_front.c_str());
    api_->RegisterFront(front);
    api_->Init();
    if (log_) log_->log(2, lit("[md] connecting "), acct_.md_front.c_str(), lit(" api="), CThostFtdcMdApi::GetApiVersion());
}

void CtpMdEngine::post_run() {
    if (!api_) return;
    api_->RegisterSpi(nullptr);
    api_->Release();
    api_ = nullptr;
    logged_in_.store(false, std::memory_order_release);
}

bool CtpMdEngine::subscribe(const char* const* codes, int n) {
    pending_sub_.clear();
    inst_ = InstMap{};
    for (int i = 0; i < n; ++i) {
        if (!codes[i] || !codes[i][0]) continue;
        pending_sub_.emplace_back(codes[i]);
        if (!inst_.add(codes[i], static_cast<uint32_t>(pending_sub_.size()))) return false;
    }
    if (!api_ || !logged_in_.load(std::memory_order_acquire) || pending_sub_.empty()) return true;
    std::vector<char*> ptrs;
    for (auto& s : pending_sub_) ptrs.push_back(s.data());
    return api_->SubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size())) == 0;
}

void CtpMdEngine::OnFrontConnected() {
    CThostFtdcReqUserLoginField req{};
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", acct_.broker_id.c_str());
    std::snprintf(req.UserID, sizeof req.UserID, "%s", acct_.investor_id.c_str());
    std::snprintf(req.Password, sizeof req.Password, "%s", acct_.password.c_str());
    const int rc = api_->ReqUserLogin(&req, ++req_);
    say_i(log_, 2, "[md] connected ReqUserLogin rc=", rc);
}

void CtpMdEngine::OnFrontDisconnected(int nReason) {
    logged_in_.store(false, std::memory_order_release);
    say_i(log_, 1, "[md] disconnected reason=", nReason);
}

void CtpMdEngine::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo, int,
                                 bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        failed_.store(true, std::memory_order_release);
        say_err(log_, "[md] login fail id=", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        return;
    }
    logged_in_.store(true, std::memory_order_release);
    if (log_) log_->log(2, lit("[md] login ok day="), pRspUserLogin ? pRspUserLogin->TradingDay : "");
    if (!pending_sub_.empty()) {
        std::vector<char*> ptrs;
        for (auto& s : pending_sub_) ptrs.push_back(s.data());
        const int rc = api_->SubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()));
        if (log_) log_->log(2, lit("[md] subscribe n="), static_cast<int>(pending_sub_.size()), lit(" rc="), rc);
    }
}

void CtpMdEngine::OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                                     CThostFtdcRspInfoField* pRspInfo, int, bool bIsLast) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        say_err(log_, "[md] sub fail id=", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        return;
    }
    if (pSpecificInstrument && log_)
        log_->log(2, lit("[md] subscribed "), pSpecificInstrument->InstrumentID, lit(" last="), bIsLast ? 1 : 0);
}

void CtpMdEngine::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pData) {
    if (!pData || !desk()) return;

    uint64_t t0 = 0;
    (void)cycles_ordered(t0);

    const uint32_t name = inst_.find(pData->InstrumentID);
    if (name == 0) return;

    const double bid = bad_px(pData->BidPrice1) ? 0 : pData->BidPrice1;
    const double ask = bad_px(pData->AskPrice1) ? 0 : pData->AskPrice1;
    const double last = bad_px(pData->LastPrice) ? 0 : pData->LastPrice;

    Quote q{};
    q.name = name;
    q.bid_px = px_x10000(bid);
    q.ask_px = px_x10000(ask);
    q.last_px = px_x10000(last);
    q.bid_qty = pData->BidVolume1;
    q.ask_qty = pData->AskVolume1;
    q.exch_us = hhmmss_ms_to_us(pData->UpdateTime, pData->UpdateMillisec);
    q.local_us = now_us();
    if (!write_quote(q)) return;
    if (tick_fn_) tick_fn_(tick_user_, name, q, t0);
}

} // namespace pitlane
