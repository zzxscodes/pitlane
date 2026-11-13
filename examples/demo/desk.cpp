#include "pitlane/pitlane.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>

// Shared desk: names, a normalized order, cash, one quote, and one session beat.

namespace {

int fail(const char* what) {
    std::fprintf(stderr, "desk: %s\n", what);
    return 1;
}

int g_quotes = 0;
int g_timers = 0;

void on_quote(void*, const pitlane::Quote&) { ++g_quotes; }
void on_timer(void*, uint64_t, int64_t) { ++g_timers; }

void on_log(int, const char* line, void*) { std::printf("log %s\n", line); }

}  // namespace

int main() {
    pitlane::NameBook names;
    uint32_t id = 0;
    if (!names.insert("IF2412", 1, &id)) return fail("name");
    names.set_session(id, 9 * 60 + 30, 15 * 60);
    names.set_quote(id, 100, 101, 100);
    if (!names.in_session(id, 10 * 60)) return fail("session");

    pitlane::GateForm form{};
    std::snprintf(form.symbol, sizeof form.symbol, "600000.SH");
    form.side = 1;
    form.off = static_cast<int8_t>(pitlane::Off::Open);
    form.px_mode = static_cast<int8_t>(pitlane::PxMode::Limit);
    form.px = 1000;
    form.qty = 100;
    const pitlane::GateResult gate = pitlane::normalize_gate(form);
    if (gate.why != pitlane::Why::None || gate.product != pitlane::Product::Stock) return fail("normalize");

    pitlane::CashLine rows[4]{};
    pitlane::CashBoard cash(rows, 4);
    pitlane::CashLine* row = cash.upsert(1, "stock", "acct", "desk", "book");
    if (!row) return fail("cash");
    pitlane::CashBoard::credit(*row, 1000);
    if (!pitlane::CashBoard::reserve(*row, 400)) return fail("reserve");

    const char* path = "/tmp/pitlane-example-desk.bin";
    ::unlink(path);
    pitlane::MapInfo map{};
    if (pitlane::map_create(path, pitlane::desk_bytes(), &map) != 0) return fail("map");
    pitlane::Desk* desk = pitlane::desk_view(&map, 1);
    if (!desk) return fail("view");

    pitlane::IdMint mint(1, 1);
    pitlane::Quote quote{};
    quote.name = id;
    quote.bid_px = 100;
    quote.ask_px = 101;
    quote.exch_us = 10 * 3600 * 1000000ll;
    const uint64_t loop = 1ull << pitlane::kLoopReader;
    if (!desk->quotes.publish(quote, loop)) return fail("quote");

    pitlane::Intent intent{};
    intent.tag = mint.next(4, 10 * 3600);
    intent.name = id;
    intent.px = gate.px;
    intent.qty = gate.qty;
    intent.side = static_cast<uint8_t>(pitlane::Side::Buy);
    intent.act = static_cast<uint8_t>(pitlane::Act::Enter);
    intent.style = static_cast<uint8_t>(pitlane::Style::Day);
    if (!desk->intents.publish(intent, 0)) return fail("intent");
    pitlane::Intent batch[2] = {intent, intent};
    batch[0].tag = mint.next(4, 10 * 3600);
    batch[1].tag = mint.next(4, 10 * 3600);
    if (desk->intents.publish_batch(batch, 2, 0) != 2) return fail("batch");

    pitlane::Pulse pulse{};
    pulse.name = id;
    pulse.stamp_us = quote.exch_us;
    pulse.value = 1;
    std::snprintf(pulse.topic, sizeof pulse.topic, "mid");
    pitlane::PulseBoard board;
    if (!board.watch(id, "mid", 0, false) || !board.offer(pulse)) return fail("pulse");

    pitlane::WantMask mask;
    mask.filter(true);
    mask.allow(id, true);
    if (!mask.accept(id)) return fail("mask");
    pitlane::RawTick raw[2]{};
    raw[0].tag = 'S';
    raw[0].px = 10000;
    raw[1].px = 25000;
    raw[1].qty = 3;
    pitlane::RawTick ticks[2]{};
    if (pitlane::take_ticks(raw, 2, quote.exch_us, 10000, ticks, 2) != 1 || ticks[0].px != 2) return fail("ticks");

    pitlane::Dict dict;
    if (dict.intern("mid") != 1 || std::strcmp(dict.text(1), "mid") != 0) return fail("dict");

    pitlane::Log log(2);
    log.bind(on_log, nullptr);
    log.write(2, "desk ready");

    pitlane::DeskLoop session;
    if (!session.add_timer(1, quote.exch_us, 0)) return fail("timer");
    pitlane::LoopHooks hooks{};
    hooks.on_quote = on_quote;
    hooks.on_timer = on_timer;
    if (session.beat(*desk, quote.exch_us, hooks) < 2 || g_quotes != 1 || g_timers != 1) return fail("beat");

    pitlane::Intent seen{};
    bool fresh = false;
    if (!desk->intents.at(0, 0, seen, &fresh) || seen.tag != intent.tag) return fail("read");

    std::printf("desk name=%u tag=%llu free=%lld quotes=%d\n", id, static_cast<unsigned long long>(seen.tag),
                static_cast<long long>(row->free_amt), g_quotes);
    pitlane::map_release(&map);
    ::unlink(path);
    return 0;
}
