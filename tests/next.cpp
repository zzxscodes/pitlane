#include "pitlane/pitlane.hpp"

#include <cstdio>
#include <cstring>

namespace {

int g_fails = 0;

void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

void check_mold_shed() {
    pitlane::IdSlip ids;
    expect(ids.issue() == 1 && ids.undo() && ids.issue() == 1, "undo the unsent id");
    expect(ids.issue() == 2 && ids.undo() && ids.issue() == 2, "only the latest id is returned");
    ids.keep();
    expect(!ids.undo(), "a kept id stays issued");

    uint8_t storage[32]{};
    const uint8_t prefix[] = {1, 2, 3, 4, 5, 6, 7, 8};
    pitlane::WireMold mold;
    expect(mold.seal(storage, 32, prefix, 8) && mold.put_i64(0, 42), "patch one field");
    expect(storage[0] == 42 && storage[1] == 0, "little endian");

    uint8_t frame[24]{};
    frame[0] = 12;
    frame[2] = 7;
    frame[8] = 9;
    pitlane::BlockHead head{};
    const uint8_t* body = nullptr;
    expect(pitlane::read_block(frame, 24, 8, head, &body) && head.tmpl == 7 && body[0] == 9, "wider block");
    expect(!pitlane::read_block(frame, 24, 16, head, &body), "older reader rejects a short block");
    expect(pitlane::block_end(frame, 24) == frame + 20, "skip the extra bytes");

    pitlane::StayShed shed(5, 10);
    expect(!shed.drop(0, 6) && !shed.drop(0, 10), "window has not closed");
    expect(shed.drop(0, 16) && shed.dropping(), "sustained lateness");
    expect(!shed.drop(20, 20) && !shed.dropping(), "recovered");
}

void check_chunk_ticket() {
    expect(pitlane::floor_chunk(-65, 64) == -128, "negative block");
    pitlane::ChunkMap<64, 2> book;
    expect(book.write(3, 4) && book.write(70, 8), "two blocks");
    int64_t qty = 0;
    expect(book.read(3, &qty) && qty == 4, "first tick");
    expect(!book.write(140, 1) && book.read(3, &qty) && qty == 4, "full directory keeps old data");

    pitlane::ChainNode pool[4]{};
    pitlane::ChainLevel level;
    pitlane::ChainLoc first{};
    pitlane::ChainLoc second{};
    expect(pitlane::chain_link(pool, 4, level, 0, 5, &first), "link first");
    expect(pitlane::chain_link(pool, 4, level, 1, 7, &second) && level.total == 12, "link second");
    expect(pitlane::chain_zero(pool, 4, level, first) && level.total == 7, "zero stays linked");
    expect(pitlane::chain_take(pool[0], 3) == 0, "zero quantity is skipped");
    expect(pitlane::chain_unlink(pool, 4, level, first), "unlink");
    expect(!pitlane::chain_unlink(pool, 4, level, first), "old handle");

    pitlane::Ticket ticket;
    ticket.tag = 9;
    ticket.phase = pitlane::Phase::Working;
    ticket.qty = 10;
    ticket.px = 100;
    pitlane::TicketMove move{};
    expect(pitlane::ticket_pull(ticket, move) && ticket.phase == pitlane::Phase::PullWait, "pull");
    expect(pitlane::ticket_fill(ticket, 2) && ticket.phase == pitlane::Phase::PullWait, "fill does not finish a pull");
    expect(!pitlane::ticket_rewind(ticket, move), "a newer fill blocks rewind");

    pitlane::Ticket fresh = ticket;
    fresh.phase = pitlane::Phase::Working;
    fresh.filled = 0;
    fresh.rev = 0;
    expect(pitlane::ticket_amend(fresh, 110, 8, move) && fresh.px == 110, "amend");
    expect(pitlane::ticket_rewind(fresh, move) && fresh.px == 100 && fresh.qty == 10, "rewind");
}

void check_parent_recon() {
    pitlane::SlicePlan plan{};
    expect(pitlane::slice_open(plan, 10, 9, 3, 1, 100), "open slices");
    pitlane::SliceCut cut{};
    expect(pitlane::slice_poll(plan, 100, cut) && cut.qty == 3 && !cut.last, "first slice");
    expect(!pitlane::slice_poll(plan, 101, cut), "too early");
    expect(pitlane::slice_poll(plan, 103, cut) && cut.qty == 3, "second slice");
    expect(pitlane::slice_poll(plan, 106, cut) && cut.qty == 4 && cut.last, "last slice keeps the remainder");
    pitlane::SlicePlan again{};
    expect(pitlane::slice_open(again, 10, 9, 3, 1, 100), "open again");
    expect(pitlane::slice_poll(again, 100, cut) && pitlane::slice_poll(again, 103, cut), "two children");
    expect(pitlane::slice_restore(again, 2) && again.pool == 6 && again.scheduled == 4, "return remainder");
    expect(pitlane::slice_late(again, 8) == 2 && again.scheduled == 2 && again.pool == 0, "late fill nets the next child");

    pitlane::BasketNote note{};
    expect(pitlane::basket_make(3, pitlane::BasketOp::Pause, "grid", 20, 64, note) && note.bytes == 20, "basket header");
    expect(!pitlane::basket_make(3, pitlane::BasketOp::Pause, "grid", 80, 64, note), "payload stays outside");

    pitlane::BookOrder order{};
    order.tag = 4;
    pitlane::SeenIds<64> seen;
    pitlane::ReconEvent ev[4]{};
    expect(pitlane::recon_fill(order, seen, 11, 4, 100, 10, ev, 4) == 2, "create from a fill");
    expect(ev[0].kind == pitlane::ReconKind::Ack && ev[1].kind == pitlane::ReconKind::Fill, "ack then fill");
    expect(pitlane::recon_fill(order, seen, 11, 4, 100, 10, ev, 4) == 1 && ev[0].kind == pitlane::ReconKind::Dup, "same fill");
    expect(pitlane::recon_fill(order, seen, 12, 20, 100, 10, ev, 4) == 2, "rest plus extra");
    expect(ev[0].kind == pitlane::ReconKind::Fill && ev[0].qty == 6 && ev[1].kind == pitlane::ReconKind::Over && order.filled == 10,
           "overfill is separate");
}

void check_flight_bar_corp() {
    pitlane::FlightQueue<4> queue;
    expect(queue.push(pitlane::Flight{1, 1, 10, pitlane::FlightAct::Enter}), "enter");
    expect(queue.push(pitlane::Flight{2, 2, 5, pitlane::FlightAct::Pull}), "other name");
    pitlane::Flight out[4]{};
    expect(queue.take(1, 10, out, 4) == 1 && out[0].tag == 1 && queue.size() == 1, "other name stays");
    expect(queue.take(2, 4, out, 4) == 0 && queue.take(2, 5, out, 4) == 1, "not due yet");

    pitlane::BarFold bars(60);
    pitlane::Bar closed{};
    expect(bars.on_tick(10, 100, 1, true, &closed) && bars.current().open == 100, "open bar");
    expect(bars.on_tick(20, 110, 2, true, &closed) && bars.current().high == 110 && bars.current().vol == 3, "update");
    expect(!bars.on_tick(30, 90, 1, false, &closed) && bars.current().close == 110, "session flag skips the tick");
    expect(bars.on_tick(70, 80, 1, true, &closed) && closed.open == 100 && closed.close == 110, "next bucket");
    expect(bars.current().start_us == 60 && bars.current().open == 80, "new bar");

    const pitlane::CorpAct acts[] = {
        {20240102, 1, 2, 100, 0, 0},
        {20240103, 1, 1, 100, 10, 0},
    };
    pitlane::PxMul factor{};
    int64_t px = 0;
    expect(pitlane::mul_fold(acts, 2, 20240101, factor) && pitlane::scale_px(50, factor, px) && px == 50, "before the split");
    expect(pitlane::mul_fold(acts, 2, 20240102, factor) && pitlane::scale_px(50, factor, px) && px == 100, "after the split");
    expect(pitlane::mul_fold(acts, 2, 20240103, factor) && pitlane::scale_px(50, factor, px) && px == 100, "price-only dividend");
    int64_t num = 0;
    int64_t den = 1;
    expect(pitlane::traded_move(100, 50, 1, 2, num, den) && num == 0, "the split itself is not a traded move");
    expect(pitlane::traded_move(100, 60, 1, 2, num, den) && num == 1 && den == 5, "traded move after the jump");
    pitlane::CorpAct income{20240103, 1, 1, 100, 10, 1};
    pitlane::PxMul step{};
    expect(pitlane::act_mul(income, step) && step.num == 10 && step.den == 9, "income dividend");
}

}  // namespace

int main() {
    check_mold_shed();
    check_chunk_ticket();
    check_parent_recon();
    check_flight_bar_corp();
    if (g_fails) {
        std::fprintf(stderr, "%d next check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
