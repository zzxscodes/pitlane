#pragma once

#include "pitlane/judge.hpp"
#include "pitlane/lap.hpp"
#include "pitlane/life.hpp"
#include "pitlane/textbuf.hpp"

#include <cstdint>
#include <type_traits>

namespace pitlane {

// One order, kept in storage the caller owns.
// The life table still decides which event is legal. A refused event leaves the sheet unchanged.
// Timestamps stay unset at -1. The first non-negative stamp at a node is kept.

enum class SheetOp : uint8_t {
    Stage = 1,
    Accept = 2,
    Fill = 3,
    Finish = 4,
    AskPull = 5,
    ConfirmPull = 6,
    Reject = 7,
    Expire = 8,
    Resume = 9,
    ResumePartial = 10
};

struct Sheet {
    uint64_t local{0};
    uint64_t counter{0};
    int64_t px{0};
    int64_t qty{0};
    int64_t filled{0};
    int64_t left{0};
    int64_t send_ns{-1};
    int64_t ack_ns{-1};
    int64_t done_ns{-1};
    Cause cause{Cause::Ok};
    Phase phase{Phase::None};
    uint8_t side{0};
};

static_assert(std::is_trivially_copyable<Sheet>::value, "sheet");

inline bool sheet_open(Sheet& sheet, uint64_t local, uint8_t side, int64_t px, int64_t qty) noexcept {
    if (local == 0 || !known_side(side) || px <= 0 || qty <= 0) return false;
    Sheet next;
    next.local = local;
    next.side = side;
    next.px = px;
    next.qty = qty;
    next.left = qty;
    sheet = next;
    return true;
}

inline bool sheet_bind(Sheet& sheet, uint64_t counter) noexcept {
    if (counter == 0 || sheet.local == 0) return false;
    if (sheet.counter != 0 && sheet.counter != counter) return false;
    sheet.counter = counter;
    return true;
}

inline bool sheet_event(SheetOp op, LifeEvent& out) noexcept {
    switch (op) {
        case SheetOp::Stage: out = LifeEvent::Submit; return true;
        case SheetOp::Accept: out = LifeEvent::Accept; return true;
        case SheetOp::Fill: out = LifeEvent::Fill; return true;
        case SheetOp::Finish: out = LifeEvent::Finish; return true;
        case SheetOp::AskPull: out = LifeEvent::AskPull; return true;
        case SheetOp::ConfirmPull: out = LifeEvent::ConfirmPull; return true;
        case SheetOp::Reject: out = LifeEvent::Reject; return true;
        case SheetOp::Expire: out = LifeEvent::Expire; return true;
        case SheetOp::Resume: out = LifeEvent::Resume; return true;
        case SheetOp::ResumePartial: out = LifeEvent::ResumePartial; return true;
    }
    return false;
}

inline bool sheet_apply(Sheet& sheet, SheetOp op, int64_t qty, int64_t stamp_ns, Cause cause) noexcept {
    LifeEvent ev{};
    if (sheet.local == 0 || !sheet_event(op, ev)) return false;
    const Phase next = life_next(sheet.phase, ev);
    if (next == Phase::None) return false;
    if (op == SheetOp::Fill && (qty <= 0 || qty > sheet.left)) return false;
    Sheet updated = sheet;
    updated.phase = next;
    if (op == SheetOp::Fill) {
        updated.filled += qty;
        updated.left -= qty;
    }
    if (op == SheetOp::Reject) updated.cause = cause;
    if (stamp_ns >= 0) {
        if (op == SheetOp::Stage && updated.send_ns < 0) updated.send_ns = stamp_ns;
        if (op == SheetOp::Accept && updated.ack_ns < 0) updated.ack_ns = stamp_ns;
        if ((op == SheetOp::Finish || op == SheetOp::ConfirmPull || op == SheetOp::Reject || op == SheetOp::Expire) &&
            updated.done_ns < 0) {
            updated.done_ns = stamp_ns;
        }
    }
    sheet = updated;
    return true;
}

inline bool sheet_ack_ns(const Sheet& sheet, int64_t& out) noexcept {
    return order_to_ack(sheet.send_ns, sheet.ack_ns, out);
}

inline bool sheet_done_ns(const Sheet& sheet, int64_t& out) noexcept {
    return tick_to_trade(sheet.send_ns, sheet.done_ns, out);
}

template <int Cap>
bool sheet_line(const Sheet& sheet, TextBuf<Cap>& out) noexcept {
    if (sheet.local > static_cast<uint64_t>(INT64_MAX)) return false;
    TextBuf<Cap> line;
    if (!line.put("id=") || !line.put_i64(static_cast<int64_t>(sheet.local)) || !line.put(" side=") ||
        !line.put_i64(sheet.side) || !line.put(" phase=") || !line.put(phase_text(sheet.phase)) || !line.put(" cause=") ||
        !line.put(cause_text(sheet.cause)) || !line.put(" qty=") || !line.put_i64(sheet.qty) || !line.put(" filled=") ||
        !line.put_i64(sheet.filled) || !line.put(" left=") || !line.put_i64(sheet.left)) {
        return false;
    }
    out.clear();
    return out.put(line.c_str());
}

// One pass over the checks a new or pulled order meets before the caller sends it.
// A null piece is skipped. The flow window changes only after every earlier check has passed.
// The call does not send the order.

struct GateAsk {
    Act act{Act::Enter};
    PxMode mode{PxMode::Limit};
    Off off{Off::Open};
    int64_t px{0};
    int64_t qty{0};
    uint8_t side{0};
    const LaneRule* lane{nullptr};
    const NameList* names{nullptr};
    const char* symbol{nullptr};
    FlowWindow* flow{nullptr};
    int64_t now{0};
    int64_t flow_span{0};
    uint64_t flow_limit{0};
    const WashRow* wash{nullptr};
    int wash_n{0};
    uint32_t acct{0};
    uint32_t group{0};
    uint32_t name_id{0};
    int64_t last{0};
    int64_t dev_den{0};
    int64_t dev_cap{-1};
};

inline Cause gate_ask(const GateAsk& ask) noexcept {
    if (ask.act != Act::Enter && ask.act != Act::Pull) return Cause::Blocked;
    if (ask.act == Act::Pull && ask.qty < 0) return Cause::BadQty;
    if (ask.act == Act::Enter) {
        if (ask.mode == PxMode::Limit && ask.px <= 0) return Cause::BadPrice;
        if (ask.mode != PxMode::Limit && ask.mode != PxMode::Market) return Cause::BadPrice;
        if (ask.qty <= 0) return Cause::BadQty;
    }
    if (ask.names != nullptr) {
        if (ask.symbol == nullptr || ask.symbol[0] == 0) return Cause::BadSymbol;
        if (ask.names->has(ask.symbol)) return Cause::Blocked;
    }
    if (ask.lane != nullptr && !lane_accepts(*ask.lane, ask.mode, ask.off, ask.act)) {
        if (ask.act == Act::Pull) return Cause::PullCap;
        if (ask.mode == PxMode::Limit && ask.lane->limit == 0) return Cause::BadPrice;
        if (ask.mode == PxMode::Market && ask.lane->market == 0) return Cause::BadPrice;
        if (ask.mode != PxMode::Limit && ask.mode != PxMode::Market) return Cause::BadPrice;
        return Cause::Blocked;
    }
    if (ask.act == Act::Enter && ask.mode == PxMode::Limit && ask.dev_cap >= 0) {
        const Cause dev = deviation_cause(ask.side == static_cast<uint8_t>(Side::Buy), ask.px, ask.last, ask.dev_den, ask.dev_cap);
        if (dev != Cause::Ok) return dev;
    }
    if (ask.act == Act::Enter && ask.wash != nullptr && ask.wash_n > 0) {
        const Cause wash = wash_cause(ask.wash, ask.wash_n, ask.acct, ask.group, ask.name_id, ask.side, ask.px);
        if (wash != Cause::Ok) return wash;
    }
    if (ask.flow != nullptr && ask.flow_limit > 0) {
        const Cause flow_cause = ask.act == Act::Pull ? Cause::PullFlow : Cause::OrderFlow;
        const uint64_t n = static_cast<uint64_t>(ask.flow->count(ask.now, ask.flow_span)) + 1ull;
        if (n >= ask.flow_limit) return flow_cause;
        if (!ask.flow->add(ask.now, ask.flow_span)) return flow_cause;
    }
    return Cause::Ok;
}

}  // namespace pitlane
