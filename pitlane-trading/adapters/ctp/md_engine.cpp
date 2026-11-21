#include "md_engine.hpp"

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

int64_t now_us() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000ll + ts.tv_nsec / 1000;
}

// 定长字段拷贝：最多写入 cap-1 字节并补 NUL，等价于 snprintf("%s") 但不走格式化，
// 也不会读到源字段边界之外。
void copy_text(char* dst, std::size_t cap, const char* src) noexcept {
    std::size_t n = 0;
    while (n + 1 < cap && src[n] != '\0') ++n;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

} // namespace

CtpMdEngine::CtpMdEngine(Account acct, trading::CtpCfg cfg) : acct_(std::move(acct)), cfg_(std::move(cfg)) {}

CtpMdEngine::~CtpMdEngine() { post_run(); }

bool CtpMdEngine::set_up() {
    if (!MdEngine::set_up()) return false;
    pending_sub_ = acct_.instruments;
    inst_ = InstMap{};
    for (std::size_t i = 0; i < pending_sub_.size(); ++i) {
        if (!inst_.add(pending_sub_[i].c_str(), static_cast<uint32_t>(i + 1))) return false;
    }
    // 逐跳预算：解码 4us、建簿 4us、分发（含逐笔重建）4us，端到端 16us。
    hops_.set_budget(kHopWireIn, 2000);
    hops_.set_budget(kHopDecode, 4000);
    hops_.set_budget(kHopBook, 4000);
    hops_.set_budget(kHopSignal, 4000);
    hops_.set_total_budget(16000);
    scale_ok_ = cycle_warm(scale_, 10);
    // 控制台视图作为第一个订阅者挂在分发点上，后续订阅者不再影响它。
    (void)fan_.add(this, &CtpMdEngine::live_snap, &CtpMdEngine::live_print);
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
    std::snprintf(front, sizeof front, "%s", cfg_.md_front.c_str());
    api_->RegisterFront(front);
    api_->Init();
    if (log_) log_->log(2, lit("[md] connecting "), cfg_.md_front.c_str(), lit(" api="), CThostFtdcMdApi::GetApiVersion());
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
    std::snprintf(req.BrokerID, sizeof req.BrokerID, "%s", cfg_.broker_id.c_str());
    std::snprintf(req.UserID, sizeof req.UserID, "%s", cfg_.investor_id.c_str());
    std::snprintf(req.Password, sizeof req.Password, "%s", cfg_.password.c_str());
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
    uint64_t t1 = 0;
    uint64_t t2 = 0;
    uint64_t t3 = 0;
    (void)cycles_ordered(t0);

    const uint32_t name = inst_.find(pData->InstrumentID);
    if (name == 0) return;

    // 1) 原始字段落进定长结构。无效价（DBL_MAX）原样保留，由解码器统一转成 0。
    // 每个字段都会写满，省略零初始化以省掉每帧约 336B 的 memset。
    CtpRaw raw;
    copy_text(raw.instrument, sizeof raw.instrument, pData->InstrumentID);
    copy_text(raw.exchange, sizeof raw.exchange, pData->ExchangeID);
    copy_text(raw.trading_day, sizeof raw.trading_day, pData->TradingDay);
    copy_text(raw.action_day, sizeof raw.action_day, pData->ActionDay);
    copy_text(raw.update_time, sizeof raw.update_time, pData->UpdateTime);
    raw.update_ms = pData->UpdateMillisec;
    raw.last_px = pData->LastPrice;
    raw.pre_settle = pData->PreSettlementPrice;
    raw.pre_close = pData->PreClosePrice;
    raw.open_px = pData->OpenPrice;
    raw.high_px = pData->HighestPrice;
    raw.low_px = pData->LowestPrice;
    raw.volume = pData->Volume;
    raw.turnover = pData->Turnover;
    raw.open_interest = pData->OpenInterest;
    raw.upper_limit = pData->UpperLimitPrice;
    raw.lower_limit = pData->LowerLimitPrice;
    raw.bid_px[0] = pData->BidPrice1;
    raw.bid_px[1] = pData->BidPrice2;
    raw.bid_px[2] = pData->BidPrice3;
    raw.bid_px[3] = pData->BidPrice4;
    raw.bid_px[4] = pData->BidPrice5;
    raw.bid_qty[0] = static_cast<double>(pData->BidVolume1);
    raw.bid_qty[1] = static_cast<double>(pData->BidVolume2);
    raw.bid_qty[2] = static_cast<double>(pData->BidVolume3);
    raw.bid_qty[3] = static_cast<double>(pData->BidVolume4);
    raw.bid_qty[4] = static_cast<double>(pData->BidVolume5);
    raw.ask_px[0] = pData->AskPrice1;
    raw.ask_px[1] = pData->AskPrice2;
    raw.ask_px[2] = pData->AskPrice3;
    raw.ask_px[3] = pData->AskPrice4;
    raw.ask_px[4] = pData->AskPrice5;
    raw.ask_qty[0] = static_cast<double>(pData->AskVolume1);
    raw.ask_qty[1] = static_cast<double>(pData->AskVolume2);
    raw.ask_qty[2] = static_cast<double>(pData->AskVolume3);
    raw.ask_qty[3] = static_cast<double>(pData->AskVolume4);
    raw.ask_qty[4] = static_cast<double>(pData->AskVolume5);
    (void)cycles_ordered(t1);
    if (scale_ok_) hops_.note(kHopWireIn, ns_of(t0, t1));

    // 2) 解码成定点快照；品种编号对齐 InstMap，后续链路只认这个编号。
    // ctp_decode 成功时写满所有被读取的字段，失败时下面直接返回。
    CtpSnap s;
    const bool ok = ctp_decode(raw, s);
    (void)cycles_ordered(t2);
    if (scale_ok_) hops_.note(kHopDecode, ns_of(t1, t2));
    if (!ok) {
        ++decode_err_;
        publish_live(now_us());
        return;
    }
    s.name = name;
    s.local_us = now_us();

    // 3) 订单簿增量维护：时间回退或长跳变时自动整簿重建。
    CtpBookEvent ev = kCtpBookOk;
    if (CtpDepthBook* book = books_.ensure(name)) (void)book->apply(s, &ev);
    ++decoded_;
    const int pi = static_cast<int>(name) - 1;
    if (pi >= 0 && pi < kMaxNames) last_ev_[pi] = static_cast<uint8_t>(ev);
    (void)cycles_ordered(t3);
    if (scale_ok_) hops_.note(kHopBook, ns_of(t2, t3));

    // 4) 逐笔重建（累计量差分）与分发。单订阅者失败不影响其它订阅者。
    CtpPrint prints[kPrintsPerSnap];
    int np = 0;
    if (pi >= 0 && pi < kMaxNames) np = prints_[pi].on_snap(s, prints, kPrintsPerSnap);
    (void)fan_.publish(s, prints, np);
    publish_live(s.local_us);
    if (scale_ok_) {
        uint64_t t4 = 0;
        (void)cycles_ordered(t4);
        hops_.note(kHopSignal, ns_of(t3, t4));
        hops_.note_total(ns_of(t0, t4));
    }

    // 5) L1 报价继续喂给既有策略链路。
    Quote q{};
    q.name = name;
    q.bid_px = s.n_bid > 0 ? s.bid[0].px : 0;
    q.ask_px = s.n_ask > 0 ? s.ask[0].px : 0;
    q.bid_qty = s.n_bid > 0 ? s.bid[0].qty : 0;
    q.ask_qty = s.n_ask > 0 ? s.ask[0].qty : 0;
    q.last_px = s.last_px;
    q.exch_us = s.exch_us;
    q.local_us = s.local_us;
    if (!write_quote(q)) return;
    if (tick_fn_) tick_fn_(tick_user_, name, q, t0);
}

bool CtpMdEngine::live_snap(void* user, const CtpSnap& s) noexcept {
    static_cast<CtpMdEngine*>(user)->mark_hot(s.name);
    return true;
}

bool CtpMdEngine::live_print(void* user, const CtpPrint& p) noexcept {
    static_cast<CtpMdEngine*>(user)->push_print(p);
    return true;
}

void CtpMdEngine::mark_hot(uint32_t name) noexcept {
    int at = -1;
    for (int i = 0; i < hot_n_; ++i) {
        if (hot_[i] == name) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        if (hot_n_ < kMarketBooks) {
            at = hot_n_++;
        } else {
            at = kMarketBooks - 1;
        }
    }
    for (int i = at; i > 0; --i) hot_[i] = hot_[i - 1];
    hot_[0] = name;
}

void CtpMdEngine::push_print(const CtpPrint& p) noexcept {
    ring_[ring_head_] = p;
    ring_head_ = (ring_head_ + 1) % static_cast<uint32_t>(kPrintRing);
    if (ring_n_ < static_cast<uint32_t>(kPrintRing)) ++ring_n_;
    ++print_total_;
}

void CtpMdEngine::publish_live(int64_t stamp_us) noexcept {
    MarketSnap s{};
    s.stamp_us = stamp_us;
    s.decoded = decoded_;
    s.decode_err = decode_err_;
    s.prints = print_total_;
    s.books = static_cast<uint32_t>(books_.count());
    s.fan_sinks = static_cast<uint32_t>(fan_.count());
    for (int i = 0; i < books_.count(); ++i) {
        const CtpBookStats& st = books_.at(i).stats();
        s.stale += st.stale;
        s.crossed += st.crossed;
        s.rebuilds += st.rebuilds;
    }
    for (int i = 0; i < hot_n_ && i < kMarketBooks; ++i) {
        const CtpDepthBook* b = books_.find(hot_[i]);
        if (!b) continue;
        CtpBookView v{};
        b->copy(v);
        MarketBook& d = s.book[i];
        d.name = v.name;
        d.status = v.status;
        d.levels = v.n_bid > v.n_ask ? v.n_bid : v.n_ask;
        d.event = hot_[i] >= 1 && hot_[i] <= static_cast<uint32_t>(kMaxNames) ? last_ev_[hot_[i] - 1] : kMarketBookOk;
        d.exch_us = v.exch_us;
        d.last_px = v.last_px;
        d.upper_limit = v.upper_limit;
        d.lower_limit = v.lower_limit;
        for (int k = 0; k < kMarketDepth; ++k) {
            d.bid[k] = MarketLevel{v.bid[k].px, v.bid[k].qty};
            d.ask[k] = MarketLevel{v.ask[k].px, v.ask[k].qty};
        }
        const CtpBookStats& st = b->stats();
        d.snaps = st.snaps;
        d.updates = st.updates;
        d.rebuilds = st.rebuilds;
        d.stale = st.stale;
        d.crossed = st.crossed;
        d.imbalance_ppm = b->imbalance_ppm();
    }
    s.print_n = ring_n_ < static_cast<uint32_t>(kMarketPrints) ? ring_n_ : static_cast<uint32_t>(kMarketPrints);
    for (uint32_t i = 0; i < s.print_n; ++i) {
        const uint32_t at = (ring_head_ + static_cast<uint32_t>(kPrintRing) - 1 - i) %
                            static_cast<uint32_t>(kPrintRing);
        const CtpPrint& p = ring_[at];
        MarketPrint& d = s.print[i];
        d.name = p.name;
        d.aggressor = p.aggressor;
        d.exch_us = p.exch_us;
        d.px = p.px;
        d.qty = p.qty;
        d.volume = p.volume;
    }
    live_.publish(s);
}

} // namespace pitlane
