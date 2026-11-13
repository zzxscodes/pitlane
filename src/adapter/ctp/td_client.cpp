#include "pitlane/adapter/ctp/td_client.hpp"

#include "pitlane/util/textutil.hpp"
#include "pitlane/util/wirelog.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace pitlane {
namespace {

bool is_accepted(char status) noexcept {
    return status == THOST_FTDC_OST_NoTradeQueueing || status == THOST_FTDC_OST_PartTradedQueueing ||
           status == THOST_FTDC_OST_AllTraded || status == THOST_FTDC_OST_PartTradedNotQueueing ||
           status == THOST_FTDC_OST_NoTradeNotQueueing || status == THOST_FTDC_OST_Canceled;
}

bool is_terminal(char status) noexcept {
    return status == THOST_FTDC_OST_AllTraded || status == THOST_FTDC_OST_Canceled ||
           status == THOST_FTDC_OST_PartTradedNotQueueing || status == THOST_FTDC_OST_NoTradeNotQueueing;
}

uint64_t to_ns(const CycleScale* scale, uint64_t cycles) noexcept {
    if (!scale) return 0;
    uint64_t ns = 0;
    if (!scale_ns(*scale, cycles, ns)) return 0;
    return ns;
}

void say_i(WireLog* log, int level, const char* text, int v) noexcept {
    if (log) log->log(level, lit(text), v);
}

void say_err(WireLog* log, const char* what, int id, const char* gbk) noexcept {
    char utf[320];
    utf[0] = 0;
    if (gbk && gbk[0] && !gbk_to_utf8(gbk, std::strlen(gbk), utf, sizeof utf)) std::snprintf(utf, sizeof utf, "%s", gbk);
    if (log) log->log(0, lit("[td] "), what, lit(" id="), id, lit(" "), utf);
}

} // namespace

CtpTdClient::CtpTdClient(Account acct) : acct_(std::move(acct)) {}

CtpTdClient::~CtpTdClient() { stop(); }

bool CtpTdClient::start() {
    api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(acct_.flow_td_dir.c_str());
    if (!api_) return false;
    api_->RegisterSpi(this);
    api_->SubscribePrivateTopic(THOST_TERT_QUICK);
    api_->SubscribePublicTopic(THOST_TERT_QUICK);
    char front[160];
    std::snprintf(front, sizeof front, "%s", acct_.td_front.c_str());
    api_->RegisterFront(front);
    api_->Init();
    if (log_) log_->log(2, lit("[td] connecting "), acct_.td_front.c_str(), lit(" api="), CThostFtdcTraderApi::GetApiVersion());
    return true;
}

void CtpTdClient::stop() {
    if (!api_) return;
    api_->RegisterSpi(nullptr);
    api_->Release();
    api_ = nullptr;
    ready_.store(false, std::memory_order_release);
}

void CtpTdClient::bind_inst(const InstMap& map) noexcept {
    name_n_ = map.n < InstMap::kMax ? map.n : InstMap::kMax;
    for (int i = 0; i < name_n_; ++i) {
        name_id_[i] = map.name[i];
        std::memcpy(name_code_[i], map.code[i], 32);
    }
}

void CtpTdClient::note_t0(uint64_t tag, uint64_t t0_cycles, int64_t t0_us, uint64_t local_ns) noexcept {
    for (int i = 0; i < kLive; ++i) {
        if (t0_used_[i]) continue;
        t0_used_[i] = 1;
        t0_slot_[i].tag = tag;
        t0_slot_[i].t0_cycles = t0_cycles;
        t0_slot_[i].t0_us = t0_us;
        t0_slot_[i].local_ns = local_ns;
        return;
    }
}

int CtpTdClient::find_t0(uint64_t tag) const noexcept {
    for (int i = 0; i < kLive; ++i) {
        if (t0_used_[i] && t0_slot_[i].tag == tag) return i;
    }
    return -1;
}

int CtpTdClient::find_live(int ref) const noexcept {
    for (int i = 0; i < kLive; ++i) {
        if (live_used_[i] && live_ref_[i] == ref) return i;
    }
    return -1;
}

void CtpTdClient::finish_ref(int ref) noexcept {
    const int i = find_live(ref);
    if (i >= 0) live_used_[i] = 0;
}

bool CtpTdClient::ok_rsp(CThostFtdcRspInfoField* info, const char* what) {
    if (!info || info->ErrorID == 0) return true;
    say_err(log_, what, info->ErrorID, info->ErrorMsg);
    if (!ready_.load(std::memory_order_acquire)) failed_.store(true, std::memory_order_release);
    return false;
}

void CtpTdClient::OnFrontConnected() {
    CThostFtdcReqAuthenticateField req{};
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", acct_.broker_id.c_str());
    std::snprintf(req.UserID, sizeof req.UserID, "%s", acct_.investor_id.c_str());
    std::snprintf(req.AppID, sizeof req.AppID, "%s", acct_.app_id.c_str());
    std::snprintf(req.AuthCode, sizeof req.AuthCode, "%s", acct_.auth_code.c_str());
    const int rc = api_->ReqAuthenticate(&req, ++req_);
    say_i(log_, 2, "[td] connected ReqAuthenticate rc=", rc);
}

void CtpTdClient::OnFrontDisconnected(int nReason) {
    ready_.store(false, std::memory_order_release);
    say_i(log_, 1, "[td] disconnected reason=", nReason);
}

void CtpTdClient::OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* pRspInfo, int, bool) {
    if (!ok_rsp(pRspInfo, "authenticate")) return;
    CThostFtdcReqUserLoginField req{};
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", acct_.broker_id.c_str());
    std::snprintf(req.UserID, sizeof req.UserID, "%s", acct_.investor_id.c_str());
    std::snprintf(req.Password, sizeof req.Password, "%s", acct_.password.c_str());
    const int rc = api_->ReqUserLogin(&req, ++req_);
    say_i(log_, 2, "[td] ReqUserLogin rc=", rc);
}

void CtpTdClient::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo, int,
                                 bool) {
    if (!ok_rsp(pRspInfo, "login")) return;
    if (pRspUserLogin) {
        front_id_ = pRspUserLogin->FrontID;
        session_id_ = pRspUserLogin->SessionID;
        max_ref_.store(std::atoi(pRspUserLogin->MaxOrderRef), std::memory_order_relaxed);
        if (log_)
            log_->log(2, lit("[td] login ok day="), pRspUserLogin->TradingDay, lit(" front="), front_id_, lit(" session="),
                      session_id_);
    }
    CThostFtdcSettlementInfoConfirmField req{};
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", acct_.broker_id.c_str());
    std::snprintf(req.InvestorID, sizeof req.InvestorID, "%s", acct_.investor_id.c_str());
    const int rc = api_->ReqSettlementInfoConfirm(&req, ++req_);
    say_i(log_, 2, "[td] ReqSettlementInfoConfirm rc=", rc);
}

void CtpTdClient::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*, CThostFtdcRspInfoField* pRspInfo,
                                             int, bool) {
    if (!ok_rsp(pRspInfo, "settlement")) return;
    ready_.store(true, std::memory_order_release);
    if (log_) log_->log(2, lit("[td] ready for orders"));
}

void CtpTdClient::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo, int,
                                   bool) {
    if (!ok_rsp(pRspInfo, "RspOrderInsert") && pInputOrder) finish_ref(std::atoi(pInputOrder->OrderRef));
}

void CtpTdClient::OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo) {
    ok_rsp(pRspInfo, "ErrRtnOrderInsert");
    if (pInputOrder) finish_ref(std::atoi(pInputOrder->OrderRef));
}

void CtpTdClient::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    if (!pOrder) return;
    const int ref = std::atoi(pOrder->OrderRef);
    if (is_accepted(pOrder->OrderStatus)) {
        const int i = find_live(ref);
        if (i >= 0 && live_slot_[i].t0_cycles != 0) {
            LiveOrder lo = live_slot_[i];
            live_slot_[i].t0_cycles = 0;
            timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            const int64_t now_us = static_cast<int64_t>(ts.tv_sec) * 1000000ll + ts.tv_nsec / 1000;
            uint64_t t1 = 0;
            (void)cycles_ordered(t1);
            const uint64_t rt = to_ns(scale_, t1 > lo.t0_cycles ? t1 - lo.t0_cycles : 0);
            if (eng_ && eng_->desk()) {
                Report r{};
                r.tag = lo.tag;
                r.phase = static_cast<uint8_t>(Phase::Working);
                r.name = lo.name;
                r.stamp_us = now_us;
                r.px = static_cast<int64_t>(pOrder->LimitPrice * 10000.0 + 0.5);
                r.qty = pOrder->VolumeTotalOriginal;
                (void)eng_->desk()->reports.publish(r, eng_->report_mask);
            }
            if (accept_fn_) accept_fn_(accept_user_, lo, now_us, rt);
        }
    }
    if (is_terminal(pOrder->OrderStatus)) {
        if (log_)
            log_->log(2, lit("[td] order ref="), ref, lit(" status="), static_cast<int>(pOrder->OrderStatus),
                      lit(" traded="), pOrder->VolumeTraded, lit("/"), pOrder->VolumeTotalOriginal);
        finish_ref(ref);
    }
}

void CtpTdClient::OnRtnTrade(CThostFtdcTradeField* pTrade) {
    if (!pTrade) return;
    if (log_)
        log_->log(2, lit("[td] trade "), pTrade->InstrumentID, lit(" px="), pTrade->Price, lit(" vol="), pTrade->Volume,
                  lit(" dir="), static_cast<int>(pTrade->Direction));
}

int CtpTdClient::insert(const Intent& in) {
    if (!api_ || !ready()) return -1;
    const char* inst = nullptr;
    for (int i = 0; i < name_n_; ++i) {
        if (name_id_[i] == in.name) {
            inst = name_code_[i];
            break;
        }
    }
    if (!inst) return -2;

    LiveOrder t0{};
    const int t0i = find_t0(in.tag);
    if (t0i >= 0) {
        t0 = t0_slot_[t0i];
        t0_used_[t0i] = 0;
    }

    const int ref = max_ref_.fetch_add(1, std::memory_order_relaxed) + 1;
    CThostFtdcInputOrderField req{};
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", acct_.broker_id.c_str());
    std::snprintf(req.InvestorID, sizeof req.InvestorID, "%s", acct_.investor_id.c_str());
    std::snprintf(req.InstrumentID, sizeof req.InstrumentID, "%s", inst);
    std::snprintf(req.OrderRef, sizeof req.OrderRef, "%d", ref);
    req.Direction = (in.side == static_cast<uint8_t>(Side::Sell)) ? THOST_FTDC_D_Sell : THOST_FTDC_D_Buy;
    req.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
    req.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    req.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    req.LimitPrice = static_cast<double>(in.px) / 10000.0;
    req.VolumeTotalOriginal = static_cast<int>(in.qty);
    req.TimeCondition = THOST_FTDC_TC_IOC;
    req.VolumeCondition = THOST_FTDC_VC_AV;
    req.MinVolume = 1;
    req.ContingentCondition = THOST_FTDC_CC_Immediately;
    req.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;

    uint64_t gate = 0;
    const int rc = api_->ReqOrderInsert(&req, ++req_);
    (void)cycles_ordered(gate);
    if (rc != 0) return rc;

    int slot = -1;
    for (int i = 0; i < kLive; ++i) {
        if (!live_used_[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -3;
    live_used_[slot] = 1;
    live_ref_[slot] = ref;
    live_slot_[slot] = t0;
    live_slot_[slot].tag = in.tag;
    live_slot_[slot].name = in.name;
    live_slot_[slot].gate_ns = to_ns(scale_, (t0.t0_cycles && gate > t0.t0_cycles) ? gate - t0.t0_cycles : 0);
    std::memcpy(live_slot_[slot].instrument, inst, 32);
    return 0;
}

int CtpTdClient::cancel(uint64_t, uint32_t) { return -1; }

} // namespace pitlane
