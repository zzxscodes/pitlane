#include "pitlane/pitlane.hpp"

#include <cstdio>

// Storage the caller keeps: an id, a wire image, a parent slice, and a venue fill.

int fail(const char* what) {
    std::fprintf(stderr, "own: %s\n", what);
    return 1;
}

int main() {
    pitlane::IdSlip ids;
    if (ids.issue() != 1 || !ids.undo() || ids.issue() != 1) return fail("id");
    ids.keep();
    if (ids.undo()) return fail("kept");

    uint8_t storage[32]{};
    const uint8_t prefix[] = {1, 2, 3, 4, 5, 6, 7, 8};
    pitlane::WireMold mold;
    if (!mold.seal(storage, 32, prefix, 8) || !mold.put_i64(0, 42) || storage[0] != 42) return fail("mold");

    pitlane::StayShed shed(5, 10);
    if (shed.drop(0, 10) || !shed.drop(0, 20)) return fail("shed");

    pitlane::ChunkMap<64, 2> chunks;
    int64_t qty = 0;
    if (!chunks.write(3, 4) || !chunks.read(3, &qty) || qty != 4) return fail("chunk");

    pitlane::Ticket ticket;
    ticket.tag = 9;
    ticket.phase = pitlane::Phase::Working;
    ticket.qty = 10;
    ticket.px = 100;
    pitlane::TicketMove move{};
    if (!pitlane::ticket_pull(ticket, move) || ticket.phase != pitlane::Phase::PullWait) return fail("ticket");

    pitlane::SlicePlan plan{};
    pitlane::SliceCut cut{};
    if (!pitlane::slice_open(plan, 10, 9, 3, 1, 100) || !pitlane::slice_poll(plan, 100, cut) || cut.qty != 3)
        return fail("slice");

    pitlane::BasketNote note{};
    if (!pitlane::basket_make(3, pitlane::BasketOp::Pause, "grid", 20, 64, note) || note.bytes != 20) return fail("basket");

    pitlane::BookOrder book{};
    book.tag = 4;
    pitlane::SeenIds<64> seen;
    pitlane::ReconEvent ev[4]{};
    if (pitlane::recon_fill(book, seen, 11, 4, 100, 10, ev, 4) != 2 || ev[0].kind != pitlane::ReconKind::Ack) return fail("recon");

    pitlane::FlightQueue<4> flights;
    if (!flights.push(pitlane::Flight{1, 1, 10, pitlane::FlightAct::Enter})) return fail("flight");
    pitlane::Flight out[2]{};
    if (flights.take(1, 10, out, 2) != 1 || out[0].tag != 1) return fail("due");

    int64_t num = 0;
    int64_t den = 1;
    if (!pitlane::traded_move(100, 50, 1, 2, num, den) || num != 0) return fail("move");

    std::printf("own id=%llu slice=%lld filled=%lld\n", static_cast<unsigned long long>(ids.issue()),
                static_cast<long long>(cut.qty), static_cast<long long>(book.filled));
    return 0;
}
