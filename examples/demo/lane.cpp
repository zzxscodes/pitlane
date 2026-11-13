#include "pitlane/pitlane.hpp"

#include <cstdio>
#include <cstring>

// Checks around one order: the counter, the channel, the flow window, and a late report.

int fail(const char* what) {
    std::fprintf(stderr, "lane: %s\n", what);
    return 1;
}

int main() {
    if (pitlane::session_date(20261109, 100000, 1, nullptr) != 20261109) return fail("session");

    pitlane::BrokerWatch watch;
    pitlane::RiskCaps caps{};
    caps.max_qty = 10;
    caps.max_notional = 1000;
    caps.max_live = 2;
    pitlane::Intent in{};
    in.tag = 1;
    in.side = 1;
    in.qty = 5;
    in.px = 10;
    if (watch.screen(in, caps) != pitlane::Why::None || !watch.remember(in, 1000)) return fail("screen");

    pitlane::QueryFold fold;
    if (!fold.push(0, 3) || !fold.push(1, 3)) return fail("query");
    fold.fold();
    uint32_t accts[4]{};
    if (fold.pending(accts, 4) != 1 || accts[0] != 3) return fail("pending");

    pitlane::IdBind<4> bind;
    pitlane::CounterKey key{};
    key.kind = 1;
    std::memcpy(key.text, "OID1", 4);
    if (!bind.put(key, 42) || !bind.confirm(key)) return fail("bind");
    uint64_t local = 0;
    if (!bind.find(key, &local) || local != 42) return fail("find");

    pitlane::BindOrder order{};
    order.phase = pitlane::BindPhase::Sent;
    pitlane::bind_fill(order, 2, 10);
    pitlane::bind_status(order, pitlane::BindPhase::Acked);
    if (order.phase != pitlane::BindPhase::Partial || order.filled_qty != 2) return fail("status");

    pitlane::LaneRule rule{};
    rule.limit = 1;
    rule.pull = 1;
    if (!pitlane::lane_accepts(rule, pitlane::PxMode::Limit, pitlane::Off::Open, pitlane::Act::Enter)) return fail("lane");
    if (pitlane::cause_band(pitlane::Cause::NoCash) != pitlane::CauseBand::Purse) return fail("band");

    pitlane::NameList blocked;
    if (!blocked.add("AAA") || pitlane::list_cause(blocked, blocked, true, "AAA") != pitlane::Cause::Blocked)
        return fail("list");
    pitlane::FlowWindow flow;
    if (!flow.allow(1000, 2, 1000)) return fail("flow");

    pitlane::SeqWatch seq{};
    uint64_t missed = 0;
    if (pitlane::seq_feed(seq, 5, &missed) != 0 || pitlane::seq_feed(seq, 8, &missed) != 2) return fail("seq");

    std::printf("lane local=%llu cause=%s missed=%llu\n", static_cast<unsigned long long>(local),
                pitlane::cause_text(pitlane::Cause::NoCash), static_cast<unsigned long long>(missed));
    return 0;
}
