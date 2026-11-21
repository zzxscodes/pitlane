#include "pitlane/desk/codec.hpp"

#include <cstring>

namespace pitlane {
namespace {

bool eq(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

bool lead(const char* code, int len, const char* pfx) noexcept {
    int n = 0;
    while (pfx[n]) ++n;
    if (len < n) return false;
    for (int i = 0; i < n; ++i) {
        if (code[i] != pfx[i]) return false;
    }
    return true;
}

const char* suffix_of(const char* symbol, const char** code, int* code_len) noexcept {
    if (!symbol) return nullptr;
    const char* dot = nullptr;
    for (const char* p = symbol; *p; ++p) {
        if (*p == '.') dot = p;
    }
    if (!dot || !dot[1]) return nullptr;
    if (code) *code = symbol;
    if (code_len) *code_len = static_cast<int>(dot - symbol);
    return dot + 1;
}

}  // namespace

bool cash_product(Product product) noexcept {
    return product == Product::Stock || product == Product::Bond || product == Product::Fund || product == Product::Tech ||
           product == Product::Index || product == Product::Repo;
}

Product classify_symbol(const char* symbol) noexcept {
    const char* code = nullptr;
    int len = 0;
    const char* suf = suffix_of(symbol, &code, &len);
    if (!suf || len <= 0) return Product::Unknown;
    if (eq(suf, "DCE") || eq(suf, "SHF") || eq(suf, "CFE") || eq(suf, "CZC") || eq(suf, "INE")) return Product::Future;
    if (eq(suf, "HKSH") || eq(suf, "HKSZ") || eq(suf, "HK")) return Product::Stock;
    if (eq(suf, "SH")) {
        if (lead(code, len, "688") || lead(code, len, "689")) return Product::Tech;
        if (lead(code, len, "1000")) return Product::Option;
        if (lead(code, len, "00")) return Product::Index;
        if (lead(code, len, "0") || lead(code, len, "1")) return Product::Bond;
        if (lead(code, len, "2")) return Product::Repo;
        if (lead(code, len, "5")) return Product::Fund;
        return Product::Stock;
    }
    if (eq(suf, "SZ")) {
        if (lead(code, len, "15") || lead(code, len, "16") || lead(code, len, "18")) return Product::Fund;
        if (lead(code, len, "13")) return Product::Repo;
        if (lead(code, len, "9000")) return Product::Option;
        if (lead(code, len, "1")) return Product::Bond;
        return Product::Stock;
    }
    return Product::Unknown;
}

bool product_root(const char* symbol, char* out, int cap) noexcept {
    if (!out || cap < 2 || !symbol) return false;
    int n = 0;
    for (const char* p = symbol; *p && *p != '.'; ++p) {
        if (*p >= '0' && *p <= '9') break;
        if (n + 1 >= cap) return false;
        out[n++] = *p;
    }
    out[n] = 0;
    return n > 0;
}

CloseStyle close_style_of(const char* symbol) noexcept {
    const char* code = nullptr;
    int len = 0;
    const char* suf = suffix_of(symbol, &code, &len);
    if (suf && (eq(suf, "SHF") || eq(suf, "INE"))) return CloseStyle::SplitYday;
    return CloseStyle::Plain;
}

int wire_to_dir(int wire_side) noexcept {
    if (wire_side == 0 || wire_side == 2 || wire_side == 5 || wire_side == 6) return 0;
    if (wire_side == 1 || wire_side == 3 || wire_side == 4 || wire_side == 7) return 1;
    return -1;
}

Biz wire_to_biz(int wire_side) noexcept {
    switch (wire_side) {
        case 0: return Biz::Trade;
        case 1: return Biz::Trade;
        case 2: return Biz::MarginBuy;
        case 3: return Biz::ShortSell;
        case 4: return Biz::SellToRepay;
        case 5: return Biz::BuyToCover;
        case 6: return Biz::Subscribe;
        case 7: return Biz::Redeem;
        case 8: return Biz::CashRepay;
        case 9: return Biz::ReturnStock;
        case 10: return Biz::OddLotTransfer;
        case 11: return Biz::CollateralIn;
        case 12: return Biz::CollateralOut;
        case 13: return Biz::Lock;
        case 14: return Biz::Unlock;
        case 17: return Biz::SplitOrder;
        case 18: return Biz::MergeOrder;
        default: return Biz::None;
    }
}

GateResult normalize_gate(const GateForm& in) noexcept {
    GateResult out;
    out.qty = in.qty;
    out.px = in.px;
    out.product = classify_symbol(in.symbol);
    out.mode = static_cast<PxMode>(in.px_mode);
    if (out.mode < PxMode::Unknown || out.mode > PxMode::OwnBest) out.mode = PxMode::Unknown;
    if (out.qty <= 0) {
        out.why = Why::Qty;
        return out;
    }
    const Off req = static_cast<Off>(in.off);
    if (!off_known(req)) {
        out.why = Why::Mode;
        return out;
    }
    bool forced = force_off(req);
    Off base = base_off(req);
    if (cash_product(out.product)) {
        base = Off::Open;
        forced = false;
    } else if (out.product == Product::Future || out.product == Product::Option) {
        if (base == Off::Close || base == Off::CloseToday || base == Off::CloseYday) {
            if (close_style_of(in.symbol) == CloseStyle::SplitYday && base == Off::Close) base = Off::CloseYday;
            else if (close_style_of(in.symbol) == CloseStyle::Plain) base = Off::Close;
        }
    }
    out.off = forced ? req : base;

    int dir = -1;
    if (in.wire_side >= 0) {
        out.biz = wire_to_biz(in.wire_side);
        if (out.biz == Biz::None) {
            out.why = Why::Side;
            return out;
        }
        dir = wire_to_dir(in.wire_side);
    } else {
        if (in.biz >= 0) {
            out.biz = static_cast<Biz>(in.biz);
            if (!biz_known(out.biz)) {
                out.why = Why::Side;
                return out;
            }
        }
        if (in.side == static_cast<uint8_t>(Side::Buy)) dir = 0;
        else if (in.side == static_cast<uint8_t>(Side::Sell)) dir = 1;
    }
    // Non-directional business (collateral, lock, repayments, split, merge)
    // needs no side; everything else does.
    if (dir < 0 && biz_directional(out.biz)) {
        out.why = Why::Side;
        return out;
    }
    out.dir = static_cast<BookDir>(dir < 0 ? 0 : dir);

    if (biz_directional(out.biz) && out.mode == PxMode::Limit && out.px <= 0) {
        out.why = Why::Mode;
        return out;
    }
    if (out.mode == PxMode::Unknown && out.px > 0) out.mode = PxMode::Limit;
    return out;
}

int64_t resolve_px(PxMode mode, uint8_t side, int64_t px, int64_t bid_px, int64_t ask_px) noexcept {
    const bool buy = buy_side(side);
    switch (mode) {
        case PxMode::Market:
        case PxMode::OppBest:
            if (buy) return ask_px > 0 ? ask_px : px;
            return bid_px > 0 ? bid_px : px;
        case PxMode::OwnBest:
            if (buy) return bid_px > 0 ? bid_px : px;
            return ask_px > 0 ? ask_px : px;
        default:
            return px;
    }
}

bool final_phase(uint8_t phase) noexcept {
    return phase == static_cast<uint8_t>(Phase::Filled) || phase == static_cast<uint8_t>(Phase::Cancelled) ||
           phase == static_cast<uint8_t>(Phase::Denied) || phase == static_cast<uint8_t>(Phase::Expired);
}

bool hanging_phase(uint8_t phase) noexcept {
    return phase == static_cast<uint8_t>(Phase::Working) || phase == static_cast<uint8_t>(Phase::Partial) ||
           phase == static_cast<uint8_t>(Phase::PullWait);
}

bool running_phase(uint8_t phase) noexcept {
    return phase == static_cast<uint8_t>(Phase::Staged) || hanging_phase(phase);
}

const char* phase_text(uint8_t phase) noexcept {
    switch (static_cast<Phase>(phase)) {
        case Phase::None: return "none";
        case Phase::Staged: return "staged";
        case Phase::Working: return "working";
        case Phase::Partial: return "partial";
        case Phase::PullWait: return "pull";
        case Phase::Filled: return "filled";
        case Phase::Cancelled: return "cancelled";
        case Phase::Denied: return "denied";
        case Phase::Expired: return "expired";
    }
    return "unknown";
}

const char* why_text(Why why) noexcept {
    switch (why) {
        case Why::None: return "ok";
        case Why::Qty: return "qty";
        case Why::Side: return "side";
        case Why::Dup: return "dup";
        case Why::Stamp: return "stamp";
        case Why::Mode: return "mode";
        case Why::Name: return "name";
        case Why::Acct: return "acct";
        case Why::Band: return "band";
        case Why::Wash: return "wash";
        case Why::Rate: return "rate";
        case Why::Notional: return "notional";
        case Why::Full: return "full";
        case Why::Missing: return "missing";
        case Why::Book: return "book";
        case Why::Closed: return "closed";
        case Why::Biz: return "biz";
    }
    return "unknown";
}

int exchange_id(const char* code) noexcept {
    static constexpr const char* kCode[] = {"SH", "SZ", "DCE", "SHF", "CFE", "CZC", "INE", "HK", "HKSH", "HKSZ"};
    for (int i = 0; i < 10; ++i) {
        if (eq(code, kCode[i])) return i + 1;
    }
    return -1;
}

const char* exchange_text(int id) noexcept {
    static constexpr const char* kCode[] = {"SH", "SZ", "DCE", "SHF", "CFE", "CZC", "INE", "HK", "HKSH", "HKSZ"};
    if (id < 1 || id > 10) return "";
    return kCode[id - 1];
}

const char* holder_for(const char* symbol, const char* sh_holder, const char* sz_holder) noexcept {
    const char* code = nullptr;
    int len = 0;
    const char* suf = suffix_of(symbol, &code, &len);
    if (suf && eq(suf, "SZ")) return sz_holder ? sz_holder : "";
    return sh_holder ? sh_holder : "";
}

}  // namespace pitlane
