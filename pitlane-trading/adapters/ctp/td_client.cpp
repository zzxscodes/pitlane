#include "td_client.hpp"

#include "pitlane/desk/codec.hpp"
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

int64_t wall_us() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000ll + ts.tv_nsec / 1000;
}

// Ledger rows carry whole yuan and 1e-4 prices, as ledger_view.hpp defines.
int64_t yuan(double v) noexcept { return static_cast<int64_t>(v + (v < 0 ? -0.5 : 0.5)); }

int64_t price4(double v) noexcept {
    return static_cast<int64_t>(v * 10000.0 + (v < 0 ? -0.5 : 0.5));
}

// Speculation is the fallback for an unset or unknown flag, matching the old
// hard-coded behaviour.
TThostFtdcHedgeFlagType map_hedge(uint8_t v) noexcept {
    switch (static_cast<Hedge>(v)) {
        case Hedge::Arbitrage: return THOST_FTDC_HF_Arbitrage;
        // CTP has no covered-write flag, so a covered order books as a hedge.
        case Hedge::Hedge:
        case Hedge::Covered: return THOST_FTDC_HF_Hedge;
        default: return THOST_FTDC_HF_Speculation;
    }
}

} // namespace

CtpTdClient::CtpTdClient(Account acct, trading::CtpCfg cfg) : acct_(std::move(acct)), cfg_(std::move(cfg)) {}

CtpTdClient::~CtpTdClient() { stop(); }

bool CtpTdClient::start() {
    api_ = CThostFtdcTraderApi::CreateFtdcTraderApi(acct_.flow_td_dir.c_str());
    if (!api_) return false;
    api_->RegisterSpi(this);
    api_->SubscribePrivateTopic(THOST_TERT_QUICK);
    api_->SubscribePublicTopic(THOST_TERT_QUICK);
    char front[160];
    std::snprintf(front, sizeof front, "%s", cfg_.td_front.c_str());
    api_->RegisterFront(front);
    api_->Init();
    if (log_) log_->log(2, lit("[td] connecting "), cfg_.td_front.c_str(), lit(" api="), CThostFtdcTraderApi::GetApiVersion());
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

bool CtpTdClient::find_tag(int ref, uint64_t& tag, uint32_t& name) const noexcept {
    const int i = find_live(ref);
    if (i >= 0) {
        tag = live_slot_[i].tag;
        name = live_slot_[i].name;
        return true;
    }
    for (int k = 0; k < kRecent; ++k) {
        if (recent_[k].ref == ref) {
            tag = recent_[k].tag;
            name = recent_[k].name;
            return true;
        }
    }
    return false;
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
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
    std::snprintf(req.UserID, sizeof req.UserID, "%s", cfg_.investor_id.c_str());
    std::snprintf(req.AppID, sizeof req.AppID, "%s", cfg_.app_id.c_str());
    std::snprintf(req.AuthCode, sizeof req.AuthCode, "%s", cfg_.auth_code.c_str());
    const int rc = api_->ReqAuthenticate(&req, ++req_);
    say_i(log_, 2, "[td] connected ReqAuthenticate rc=", rc);
}

void CtpTdClient::OnFrontDisconnected(int nReason) {
    ready_.store(false, std::memory_order_release);
    say_i(log_, 1, "[td] disconnected reason=", nReason);
}

void CtpTdClient::login() noexcept {
    CThostFtdcReqUserLoginField req{};
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
    std::snprintf(req.UserID, sizeof req.UserID, "%s", cfg_.investor_id.c_str());
    std::snprintf(req.Password, sizeof req.Password, "%s", cfg_.password.c_str());
    const int rc = api_->ReqUserLogin(&req, ++req_);
    say_i(log_, 2, "[td] ReqUserLogin rc=", rc);
}

void CtpTdClient::rotate_password() noexcept {
    CThostFtdcUserPasswordUpdateField req{};
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
    std::snprintf(req.UserID, sizeof req.UserID, "%s", cfg_.investor_id.c_str());
    std::snprintf(req.OldPassword, sizeof req.OldPassword, "%s", cfg_.password.c_str());
    std::snprintf(req.NewPassword, sizeof req.NewPassword, "%s", cfg_.new_password.c_str());
    const int rc = api_->ReqUserPasswordUpdate(&req, ++req_);
    say_i(log_, 2, "[td] ReqUserPasswordUpdate rc=", rc);
}

void CtpTdClient::OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* pRspInfo, int, bool) {
    if (!ok_rsp(pRspInfo, "authenticate")) return;
    login();
}

void CtpTdClient::OnRspUserPasswordUpdate(CThostFtdcUserPasswordUpdateField*, CThostFtdcRspInfoField* pRspInfo, int,
                                          bool) {
    if (!ok_rsp(pRspInfo, "password update")) return;
    cfg_.password = cfg_.new_password;
    cfg_.new_password.clear();
    if (log_) log_->log(2, lit("[td] password updated, login again"));
    login();
}

void CtpTdClient::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo, int,
                                 bool) {
    if (pRspInfo && pRspInfo->ErrorID == 141 && !cfg_.new_password.empty() && !pw_tried_) {
        pw_tried_ = 1;
        say_err(log_, "login", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        if (log_) log_->log(1, lit("[td] password expired, rotating"));
        rotate_password();
        return;
    }
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
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
    std::snprintf(req.InvestorID, sizeof req.InvestorID, "%s", cfg_.investor_id.c_str());
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
    if (!ok_rsp(pRspInfo, "RspOrderInsert") && pInputOrder) {
        const int ref = std::atoi(pInputOrder->OrderRef);
        const int i = find_live(ref);
        if (i >= 0 && reject_fn_) reject_fn_(reject_user_, live_slot_[i].tag, pRspInfo->ErrorID, wall_us());
        finish_ref(ref);
    }
}

void CtpTdClient::OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo) {
    ok_rsp(pRspInfo, "ErrRtnOrderInsert");
    if (pInputOrder) {
        const int ref = std::atoi(pInputOrder->OrderRef);
        const int i = find_live(ref);
        if (i >= 0 && reject_fn_)
            reject_fn_(reject_user_, live_slot_[i].tag, pRspInfo ? pRspInfo->ErrorID : 0, wall_us());
        finish_ref(ref);
    }
}

void CtpTdClient::OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction, CThostFtdcRspInfoField* pRspInfo,
                                   int, bool) {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        say_err(log_, "RspOrderAction", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        return;
    }
    if (pInputOrderAction && log_) log_->log(2, lit("[td] cancel accepted ref="), pInputOrderAction->OrderRef);
}

void CtpTdClient::OnErrRtnOrderAction(CThostFtdcOrderActionField* pOrderAction, CThostFtdcRspInfoField* pRspInfo) {
    if (!pRspInfo || pRspInfo->ErrorID == 0) return;
    say_err(log_, "ErrRtnOrderAction", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
    if (pOrderAction)
        log_->log(3, lit("[td] cancel failed ref="), pOrderAction->OrderRef, lit(" err="), pRspInfo->ErrorID);
}

void CtpTdClient::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    if (!pOrder) return;
    const int ref = std::atoi(pOrder->OrderRef);
    const int li = find_live(ref);
    if (li >= 0) {
        // ExchangeID + OrderSysID are the venue handle a cancel needs.
        std::snprintf(live_slot_[li].exchange, sizeof live_slot_[li].exchange, "%s", pOrder->ExchangeID);
        std::snprintf(live_slot_[li].sys_id, sizeof live_slot_[li].sys_id, "%s", pOrder->OrderSysID);
    }
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
                r.cause = lo.cause;
                (void)eng_->desk()->reports.publish(r, eng_->report_mask);
            }
            if (accept_fn_) accept_fn_(accept_user_, lo, now_us, rt);
        }
    }
    if (is_terminal(pOrder->OrderStatus)) {
        if (log_)
            log_->log(2, lit("[td] order ref="), ref, lit(" status="), static_cast<int>(pOrder->OrderStatus),
                      lit(" traded="), pOrder->VolumeTraded, lit("/"), pOrder->VolumeTotalOriginal);
        const int i = find_live(ref);
        if (i >= 0 && done_fn_)
            done_fn_(done_user_, live_slot_[i].tag, pOrder->VolumeTraded, static_cast<int>(pOrder->OrderStatus), wall_us());
        finish_ref(ref);
    }
}

void CtpTdClient::OnRtnTrade(CThostFtdcTradeField* pTrade) {
    if (!pTrade) return;
    if (log_)
        log_->log(2, lit("[td] trade "), pTrade->InstrumentID, lit(" px="), pTrade->Price, lit(" vol="), pTrade->Volume,
                  lit(" dir="), static_cast<int>(pTrade->Direction));
    uint64_t tag = 0;
    uint32_t name = 0;
    if (trade_fn_ && find_tag(std::atoi(pTrade->OrderRef), tag, name)) {
        trade_fn_(trade_user_, tag, name, static_cast<int64_t>(pTrade->Price * 10000.0 + 0.5), pTrade->Volume,
                  wall_us());
    }
}

int CtpTdClient::insert(const Intent& in) {
    if (!api_ || !ready()) return -1;
    // Margin trading, collateral transfers, lock/unlock, and split/merge
    // orders have no futures counter message. Refuse them here instead of
    // sending an ordinary buy or sell in their place.
    if (static_cast<Biz>(in.biz) != Biz::Trade) {
        if (eng_ && eng_->desk()) {
            Report r{};
            r.tag = in.tag;
            r.name = in.name;
            r.px = in.px;
            r.qty = in.qty;
            r.left = in.qty;
            r.stamp_us = wall_us();
            r.phase = static_cast<uint8_t>(Phase::Denied);
            r.why = static_cast<uint16_t>(Why::Biz);
            r.side = in.side;
            r.biz = in.biz;
            (void)eng_->desk()->reports.publish(r, eng_->report_mask);
        }
        return -4;
    }
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
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
    std::snprintf(req.InvestorID, sizeof req.InvestorID, "%s", cfg_.investor_id.c_str());
    std::snprintf(req.InstrumentID, sizeof req.InstrumentID, "%s", inst);
    std::snprintf(req.OrderRef, sizeof req.OrderRef, "%d", ref);
    req.Direction = (in.side == static_cast<uint8_t>(Side::Sell)) ? THOST_FTDC_D_Sell : THOST_FTDC_D_Buy;
    // Off values are the CTP THOST_FTDC_OF_* digits, so each one forwards as is.
    switch (in.off) {
        case Off::Open: req.CombOffsetFlag[0] = THOST_FTDC_OF_Open; break;
        case Off::Close: req.CombOffsetFlag[0] = THOST_FTDC_OF_Close; break;
        case Off::ForceClose: req.CombOffsetFlag[0] = THOST_FTDC_OF_ForceClose; break;
        case Off::CloseToday: req.CombOffsetFlag[0] = THOST_FTDC_OF_CloseToday; break;
        case Off::CloseYday: req.CombOffsetFlag[0] = THOST_FTDC_OF_CloseYesterday; break;
        case Off::ForceOff: req.CombOffsetFlag[0] = THOST_FTDC_OF_ForceOff; break;
        case Off::LocalForceClose: req.CombOffsetFlag[0] = THOST_FTDC_OF_LocalForceClose; break;
        default: req.CombOffsetFlag[0] = THOST_FTDC_OF_Open; break;
    }
    req.CombHedgeFlag[0] = map_hedge(in.hedge);
    if (static_cast<Style>(in.style) == Style::BestFive) {
        // Five-level market order: the venue walks the best five levels and
        // cancels the balance, so no protection limit price is sent.
        req.OrderPriceType = THOST_FTDC_OPT_FiveLevelPrice;
        req.LimitPrice = 0.0;
    } else {
        const bool selling = in.side == static_cast<uint8_t>(Side::Sell);
        switch (static_cast<PxMode>(in.px_mode)) {
            case PxMode::Market:
                req.OrderPriceType = THOST_FTDC_OPT_AnyPrice;
                req.LimitPrice = 0.0;
                break;
            case PxMode::OppBest:
                req.OrderPriceType = selling ? THOST_FTDC_OPT_BidPrice1 : THOST_FTDC_OPT_AskPrice1;
                req.LimitPrice = 0.0;
                break;
            case PxMode::OwnBest:
                req.OrderPriceType = selling ? THOST_FTDC_OPT_AskPrice1 : THOST_FTDC_OPT_BidPrice1;
                req.LimitPrice = 0.0;
                break;
            default:
                req.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
                req.LimitPrice = static_cast<double>(in.px) / 10000.0;
                break;
        }
    }
    req.VolumeTotalOriginal = static_cast<int>(in.qty);
    // The desk style decides time in force: a day or passive child rests until
    // the session ends, an all-or-none child is filled-or-killed, and the
    // default immediate child trades what is shown and expires the balance.
    switch (static_cast<Style>(in.style)) {
        case Style::Day:
        case Style::Passive:
            req.TimeCondition = THOST_FTDC_TC_GFD;
            req.VolumeCondition = THOST_FTDC_VC_AV;
            break;
        case Style::All:
            req.TimeCondition = THOST_FTDC_TC_IOC;
            req.VolumeCondition = THOST_FTDC_VC_CV;
            break;
        default:
            req.TimeCondition = THOST_FTDC_TC_IOC;
            req.VolumeCondition = THOST_FTDC_VC_AV;
            break;
    }
    req.MinVolume = 1;
    req.ContingentCondition = THOST_FTDC_CC_Immediately;
    req.ForceCloseReason = in.force_reason != 0 ? static_cast<char>(in.force_reason)
                                                : THOST_FTDC_FCC_NotForceClose;

    // Register the slot before sending: a fast venue can report the ack or a
    // fill on the callback thread before ReqOrderInsert returns.
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
    live_slot_[slot].cause = in.force_reason;
    std::memcpy(live_slot_[slot].instrument, inst, 32);

    recent_[recent_pos_] = RecentRef{ref, in.tag, in.name};
    recent_pos_ = (recent_pos_ + 1) % static_cast<uint32_t>(kRecent);

    uint64_t gate = 0;
    const int rc = api_->ReqOrderInsert(&req, ++req_);
    if (rc != 0) {
        live_used_[slot] = 0;
        return rc;
    }
    (void)cycles_ordered(gate);
    live_slot_[slot].gate_ns = to_ns(scale_, (t0.t0_cycles && gate > t0.t0_cycles) ? gate - t0.t0_cycles : 0);
    return 0;
}

int CtpTdClient::exercise(const ExecNote& note) {
    if (!api_ || !ready()) return -1;
    const char* inst = nullptr;
    for (int i = 0; i < name_n_; ++i) {
        if (name_id_[i] == note.name) {
            inst = name_code_[i];
            break;
        }
    }
    if (!inst) return -2;

    const int ref = max_ref_.fetch_add(1, std::memory_order_relaxed) + 1;
    CThostFtdcInputExecOrderField req{};
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
    std::snprintf(req.InvestorID, sizeof req.InvestorID, "%s", cfg_.investor_id.c_str());
    std::snprintf(req.UserID, sizeof req.UserID, "%s", cfg_.investor_id.c_str());
    std::snprintf(req.InstrumentID, sizeof req.InstrumentID, "%s", inst);
    std::snprintf(req.ExecOrderRef, sizeof req.ExecOrderRef, "%d", ref);
    req.Volume = static_cast<int>(note.qty);
    req.RequestID = ++req_;
    req.OffsetFlag = THOST_FTDC_OF_Close;
    req.HedgeFlag = map_hedge(note.hedge);
    req.ActionType = note.kind == 2 ? THOST_FTDC_ACTP_Abandon : THOST_FTDC_ACTP_Exec;
    req.PosiDirection = THOST_FTDC_PD_Net;
    req.CloseFlag = THOST_FTDC_EOCF_NotToClose;
    const int rc = api_->ReqExecOrderInsert(&req, req.RequestID);
    if (rc != 0) return rc;

    if (eng_ && eng_->desk()) {
        Report r{};
        r.tag = note.tag;
        r.name = note.name;
        r.qty = note.qty;
        r.left = note.qty;
        r.stamp_us = note.stamp_us;
        r.phase = static_cast<uint8_t>(Phase::Working);
        (void)eng_->desk()->reports.publish(r, eng_->report_mask);
    }
    return 0;
}

int CtpTdClient::cancel(uint64_t tag, uint32_t) {
    if (!api_ || !ready()) return -1;
    int slot = -1;
    for (int i = 0; i < kLive; ++i) {
        if (live_used_[i] && live_slot_[i].tag == tag) {
            slot = i;
            break;
        }
    }
    int ref = slot >= 0 ? live_ref_[slot] : 0;
    if (ref == 0) {
        for (int k = 0; k < kRecent; ++k) {
            if (recent_[k].ref != 0 && recent_[k].tag == tag) {
                ref = recent_[k].ref;
                break;
            }
        }
    }
    if (ref == 0) return -2;

    CThostFtdcInputOrderActionField req{};
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
    std::snprintf(req.InvestorID, sizeof req.InvestorID, "%s", cfg_.investor_id.c_str());
    std::snprintf(req.UserID, sizeof req.UserID, "%s", cfg_.investor_id.c_str());
    std::snprintf(req.OrderRef, sizeof req.OrderRef, "%d", ref);
    req.FrontID = front_id_;
    req.SessionID = session_id_;
    req.ActionFlag = THOST_FTDC_AF_Delete;
    if (slot >= 0) {
        std::snprintf(req.InstrumentID, sizeof req.InstrumentID, "%s", live_slot_[slot].instrument);
        std::snprintf(req.ExchangeID, sizeof req.ExchangeID, "%s", live_slot_[slot].exchange);
        std::snprintf(req.OrderSysID, sizeof req.OrderSysID, "%s", live_slot_[slot].sys_id);
    }
    const int rc = api_->ReqOrderAction(&req, ++req_);
    say_i(log_, 2, "[td] ReqOrderAction rc=", rc);
    return rc;
}

int CtpTdClient::qry_find(int id) const noexcept {
    for (int i = 0; i < kQry; ++i) {
        if (qry_[i].id == id) return i;
    }
    return -1;
}

int CtpTdClient::qry_open(uint8_t kind, const QueryNote& note) noexcept {
    int slot = -1;
    for (int i = 0; i < kQry; ++i) {
        if (qry_[i].id == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -2;
    const int id = ++req_;
    qry_[slot] = PendQry{id, kind, note.acct, note.req, 0};
    return id;
}

void CtpTdClient::qry_reply(int id, int64_t value) noexcept {
    const int i = qry_find(id);
    if (i < 0) return;
    if (eng_) {
        if (qry_[i].kind == 2)
            (void)eng_->write_asset_rsp(qry_[i].acct, value, qry_[i].req);
        else
            (void)eng_->write_position_rsp(qry_[i].acct, value, qry_[i].req);
    }
    qry_[i] = PendQry{};
}

uint32_t CtpTdClient::name_of(const char* code) const noexcept {
    if (!code) return 0;
    for (int i = 0; i < name_n_; ++i) {
        if (std::strcmp(name_code_[i], code) == 0) return name_id_[i];
    }
    return 0;
}

void CtpTdClient::add_ledger_asset(const CThostFtdcTradingAccountField& r, int64_t stamp_us) noexcept {
    if (ledger_assets_.asset_n >= 8) return;
    LedgerAsset& a = ledger_assets_.assets[ledger_assets_.asset_n++];
    a = LedgerAsset{};
    a.tier = static_cast<uint8_t>(LedgerTier::Trader);
    a.acct = ledger_acct_;
    a.initial_equity = yuan(r.PreBalance);
    a.static_equity = yuan(r.PreBalance);
    a.dynamic_equity = yuan(r.Balance);
    a.total_cash = yuan(r.Balance);
    a.available_cash = yuan(r.Available);
    a.margin = yuan(r.CurrMargin);
    a.frozen_cash = yuan(r.FrozenCash);
    a.frozen_margin = yuan(r.FrozenMargin);
    a.accumulated_fee = yuan(r.Commission);
    a.intraday_fee = yuan(r.Commission);
    a.frozen_fee = yuan(r.FrozenCommission);
    a.realized_pnl = yuan(r.CloseProfit);
    a.unrealized_pnl = yuan(r.PositionProfit);
    a.position_pnl = yuan(r.PositionProfit);
    a.close_pnl = yuan(r.CloseProfit);
    a.credit = yuan(r.Credit);
    a.available_credit = yuan(r.Credit);
    a.asset_balance = yuan(r.Balance);
    a.fetch_balance = yuan(r.WithdrawQuota);
    a.send_us = stamp_us;
    a.stamp_us = stamp_us;
}

void CtpTdClient::add_ledger_position(const CThostFtdcInvestorPositionField& r, int64_t stamp_us) noexcept {
    const bool short_side = r.PosiDirection == THOST_FTDC_PD_Short;
    const uint8_t dir = static_cast<uint8_t>(short_side ? LedgerSide::Short : LedgerSide::Long);
    const uint32_t name = name_of(r.InstrumentID);
    int i = -1;
    for (int k = 0; k < ledger_positions_.position_n; ++k) {
        if (ledger_positions_.positions[k].name == name && ledger_positions_.positions[k].direction == dir) {
            i = k;
            break;
        }
    }
    if (i < 0) {
        if (ledger_positions_.position_n >= kLive) return;
        i = ledger_positions_.position_n++;
        LedgerPosition& p = ledger_positions_.positions[i];
        p = LedgerPosition{};
        p.tier = static_cast<uint8_t>(LedgerTier::Trader);
        p.acct = ledger_acct_;
        p.product = name;
        p.name = name;
        p.direction = dir;
        ledger_positions_.cost[i] = 0;
        ledger_positions_.open_cost[i] = 0;
    }
    LedgerPosition& p = ledger_positions_.positions[i];
    p.total += r.Position;
    p.initial_yesterday += r.YdPosition;
    p.left_yesterday += r.YdPosition;
    // Sellable excludes the frozen part, matching the counter's own position
    // semantics; the frozen quantity is reported separately.
    const int64_t frozen = short_side ? r.ShortFrozen : r.LongFrozen;
    const int64_t sellable = r.Position > frozen ? r.Position - frozen : 0;
    p.sellable += sellable;
    p.frozen += frozen;
    if (short_side) {
        p.yesterday_sell += r.YdPosition;
        p.today_sell += r.TodayPosition;
    } else {
        p.yesterday_buy += r.YdPosition;
        p.today_buy += r.TodayPosition;
    }
    ledger_positions_.cost[i] += r.PositionCost;
    ledger_positions_.open_cost[i] += r.OpenCost;
    // Average prices need the contract multiplier, which the counter does not
    // report here; emit_ledger fills avg_open_px and cost_px when it is known.
    p.last_px = price4(r.SettlementPrice);
    p.settlement_px = price4(r.SettlementPrice);
    p.pre_settlement_px = price4(r.PreSettlementPrice);
    p.margin += yuan(r.UseMargin);
    p.position_pnl += yuan(r.PositionProfit);
    p.close_pnl += yuan(r.CloseProfit);
    p.realized_pnl += yuan(r.CloseProfit);
    p.unrealized_pnl += yuan(r.PositionProfit);
    p.commission += yuan(r.Commission);
    p.stamp_us = stamp_us;
}

void CtpTdClient::emit_ledger(int kind, uint64_t req) noexcept {
    LedgerAcc& acc = kind == 1 ? ledger_assets_ : ledger_positions_;
    if (kind == 2) {
        for (int i = 0; i < acc.position_n; ++i) {
            LedgerPosition& p = acc.positions[i];
            const int64_t mult = mult_fn_ ? mult_fn_(mult_user_, p.name) : 0;
            if (mult <= 0 || p.total <= 0) continue;
            const double qty = static_cast<double>(p.total) * static_cast<double>(mult);
            if (acc.cost[i] > 0.0) p.cost_px = price4(acc.cost[i] / qty);
            if (acc.open_cost[i] > 0.0) p.avg_open_px = price4(acc.open_cost[i] / qty);
        }
    }
    // Publish the same rows to the shared desk so another process can page them
    // without the in-process snapshot. An empty batch still closes with a last
    // note, so a waiting reader never sits on a request that returned nothing.
    if (eng_) {
        if (kind == 1) {
            if (acc.asset_n == 0) {
                (void)eng_->write_ledger_asset(LedgerAsset{}, req, 0, true);
            } else {
                for (int i = 0; i < acc.asset_n; ++i) {
                    (void)eng_->write_ledger_asset(acc.assets[i], req, static_cast<uint32_t>(i),
                                                   i + 1 == acc.asset_n);
                }
            }
        } else {
            if (acc.position_n == 0) {
                (void)eng_->write_ledger_position(LedgerPosition{}, req, 0, true);
            } else {
                for (int i = 0; i < acc.position_n; ++i) {
                    (void)eng_->write_ledger_position(acc.positions[i], req, static_cast<uint32_t>(i),
                                                      i + 1 == acc.position_n);
                }
            }
        }
    }
    if (ledger_fn_) {
        if (kind == 1) {
            ledger_fn_(ledger_user_, 1, acc.assets, acc.asset_n, nullptr, 0, acc.stamp_us);
        } else {
            ledger_fn_(ledger_user_, 2, nullptr, 0, acc.positions, acc.position_n, acc.stamp_us);
        }
    }
    acc = LedgerAcc{};
}

void CtpTdClient::query_ledger() noexcept {
    if (!api_ || !ready() || !ledger_fn_) return;
    // The counter admits one query per second, so send one half per poll.
    // Firing assets and positions back to back would have the second request
    // rejected and leave that half stale.
    const bool want_assets = ledger_asset_turn_;
    ledger_asset_turn_ = !ledger_asset_turn_;
    QueryNote note{};
    note.acct = ledger_acct_;
    if (want_assets) {
        const int aid = qry_open(3, note);
        if (aid >= 0) {
            CThostFtdcQryTradingAccountField req{};
            std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
            std::snprintf(req.InvestorID, sizeof req.InvestorID, "%s", cfg_.investor_id.c_str());
            std::snprintf(req.CurrencyID, sizeof req.CurrencyID, "%s", "CNY");
            if (api_->ReqQryTradingAccount(&req, aid) != 0) {
                const int i = qry_find(aid);
                if (i >= 0) qry_[i] = PendQry{};
            }
        }
        return;
    }
    const int pid = qry_open(4, note);
    if (pid >= 0) {
        CThostFtdcQryInvestorPositionField req{};
        std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
        std::snprintf(req.InvestorID, sizeof req.InvestorID, "%s", cfg_.investor_id.c_str());
        if (api_->ReqQryInvestorPosition(&req, pid) != 0) {
            const int i = qry_find(pid);
            if (i >= 0) qry_[i] = PendQry{};
        }
    }
}

int CtpTdClient::query_asset(const QueryNote& note) {
    if (!api_ || !ready()) return -1;
    const int id = qry_open(2, note);
    if (id < 0) return id;
    CThostFtdcQryTradingAccountField req{};
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
    std::snprintf(req.InvestorID, sizeof req.InvestorID, "%s", cfg_.investor_id.c_str());
    std::snprintf(req.CurrencyID, sizeof req.CurrencyID, "%s", "CNY");
    const int rc = api_->ReqQryTradingAccount(&req, id);
    say_i(log_, 2, "[td] ReqQryTradingAccount rc=", rc);
    if (rc != 0) {
        const int i = qry_find(id);
        if (i >= 0) qry_[i] = PendQry{};
    }
    return rc;
}

int CtpTdClient::query_position(const QueryNote& note) {
    if (!api_ || !ready()) return -1;
    const int id = qry_open(1, note);
    if (id < 0) return id;
    CThostFtdcQryInvestorPositionField req{};
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
    std::snprintf(req.InvestorID, sizeof req.InvestorID, "%s", cfg_.investor_id.c_str());
    const int rc = api_->ReqQryInvestorPosition(&req, id);
    say_i(log_, 2, "[td] ReqQryInvestorPosition rc=", rc);
    if (rc != 0) {
        const int i = qry_find(id);
        if (i >= 0) qry_[i] = PendQry{};
    }
    return rc;
}

void CtpTdClient::OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                         CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    const int i = qry_find(nRequestID);
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        say_err(log_, "RspQryTradingAccount", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        if (i >= 0) {
            if (qry_[i].kind == 3) ledger_assets_ = LedgerAcc{};
            qry_[i] = PendQry{};
        }
        return;
    }
    if (i < 0) return;
    if (qry_[i].kind == 3) {
        if (pTradingAccount) {
            if (ledger_assets_.stamp_us == 0) ledger_assets_.stamp_us = wall_us();
            add_ledger_asset(*pTradingAccount, ledger_assets_.stamp_us);
        }
        if (bIsLast) {
            emit_ledger(1, qry_[i].req);
            qry_[i] = PendQry{};
        }
        return;
    }
    if (pTradingAccount) {
        const double free_amt = pTradingAccount->Available;
        qry_[i].sum = static_cast<int64_t>(free_amt < 0 ? free_amt - 0.5 : free_amt + 0.5);
    }
    if (bIsLast) {
        const int64_t sum = qry_[i].sum;
        qry_reply(nRequestID, sum);
        if (log_) log_->log(2, lit("[td] asset free="), sum);
    }
}

void CtpTdClient::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                           CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    const int i = qry_find(nRequestID);
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        say_err(log_, "RspQryInvestorPosition", pRspInfo->ErrorID, pRspInfo->ErrorMsg);
        if (i >= 0) {
            if (qry_[i].kind == 4) ledger_positions_ = LedgerAcc{};
            qry_[i] = PendQry{};
        }
        return;
    }
    if (i < 0) return;
    if (qry_[i].kind == 4) {
        if (pInvestorPosition) {
            if (ledger_positions_.stamp_us == 0) ledger_positions_.stamp_us = wall_us();
            add_ledger_position(*pInvestorPosition, ledger_positions_.stamp_us);
        }
        if (bIsLast) {
            emit_ledger(2, qry_[i].req);
            qry_[i] = PendQry{};
        }
        return;
    }
    if (pInvestorPosition) {
        const int64_t vol = static_cast<int64_t>(pInvestorPosition->Position);
        qry_[i].sum += pInvestorPosition->PosiDirection == THOST_FTDC_PD_Short ? -vol : vol;
    }
    if (bIsLast) {
        const int64_t sum = qry_[i].sum;
        qry_reply(nRequestID, sum);
        if (log_) log_->log(2, lit("[td] position net="), sum);
    }
}

} // namespace pitlane
