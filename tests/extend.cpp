#include "pitlane/pitlane.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int g_fails = 0;

void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

void check_window() {
    pitlane::PriceWindow<128> book;
    expect(!book.set(1, 100, 1), "unbound");
    expect(book.reset(100, 2), "reset");
    expect(book.set(1, 100, 3) && book.set(1, 104, 1) && book.set(2, 110, 4) && book.set(2, 108, 2), "set");
    int64_t px = 0;
    int64_t qty = 0;
    expect(book.best(1, px, qty) && px == 104 && qty == 1, "best bid");
    expect(book.set(1, 228, 9) && book.best(1, px, qty) && px == 228 && qty == 9, "far bid");
    expect(book.set(1, 228, 0) && book.best(1, px, qty) && px == 104, "far cleared");
    expect(book.set(1, 104, 0), "clear");
    expect(book.best(1, px, qty) && px == 100 && qty == 3, "next bid");
    expect(book.best(2, px, qty) && px == 108 && qty == 2, "best ask");
    expect(!book.set(1, 101, 1) && book.best(1, px, qty) && px == 100, "off step");
    expect(!book.set(1, 100 + 2 * 128, 1), "outside");
}

void check_pool() {
    pitlane::SlotPool<int, 8> pool;
    expect(pool.take(3, 30) && pool.take(9, 90) && pool.size() == 2, "take");
    expect(!pool.take(3, 1) && pool.find(3) && *pool.find(3) == 30, "dup");
    expect(pool.drop(3) && !pool.find(3) && pool.take(3, 31) && *pool.find(3) == 31, "reuse");
    expect(pool.find(9) && *pool.find(9) == 90, "other id");
    for (uint64_t id = 10; id < 16; ++id) expect(pool.take(id, static_cast<int>(id)), "fill");
    expect(!pool.take(20, 1) && pool.size() == 8, "full");
    expect(!pool.take(0, 1), "zero id");
}

void check_batch() {
    pitlane::FanRing<pitlane::Intent, 4> ring;
    pitlane::Intent items[3]{};
    items[0].tag = 1;
    items[1].tag = 2;
    items[2].tag = 3;
    expect(ring.publish_batch(items, 3, 0x1) == 3 && ring.tail() == 3, "batch");
    pitlane::Intent out{};
    bool fresh = false;
    expect(ring.at(0, 0, out, &fresh) && out.tag == 1 && ring.at(2, 0, out, &fresh) && out.tag == 3, "read batch");
    expect(ring.publish_batch(items, 2, 0x1) == 0 && ring.tail() == 3, "blocked batch");
    expect(ring.ack(0, 0) && ring.ack(1, 0) && ring.publish_batch(items, 2, 0x1) == 2, "after ack");
}

void check_line_cover_touch() {
    pitlane::TimeLine<4> line;
    uint64_t id = 0;
    expect(line.push(1) && line.push(2) && line.push(3) && line.front(id) && id == 1, "line");
    expect(!line.push(2) && line.erase(2) && line.pop(id) && id == 1 && line.pop(id) && id == 3, "erase");
    expect(line.push(4) && line.push(5) && line.push(6) && line.push(7) && !line.push(8), "line full");

    const int64_t px[] = {100, 102, 99};
    const int64_t qty[] = {5, 1, 4};
    expect(pitlane::cover_qty(px, qty, 3, true, 101, 10) == 9, "buy cover");
    expect(pitlane::cover_qty(px, qty, 3, false, 101, 10) == 1, "sell cover");
    expect(pitlane::cover_qty(px, qty, 3, true, 100, 3) == 3, "capped");

    unsigned char buf[128]{};
    buf[0] = 1;
    buf[127] = 2;
    pitlane::touch_lines(buf, sizeof buf);
    expect(buf[0] == 1 && buf[127] == 2, "touch");

    pitlane::FillRing<int, 2> ring;
    int* slot = ring.claim();
    expect(slot != nullptr, "claim");
    *slot = 7;
    expect(ring.publish(), "pub");
    slot = ring.claim();
    *slot = 8;
    expect(ring.publish() && ring.claim() == nullptr, "full");
    int got = 0;
    expect(ring.take(got) && got == 7 && ring.take(got) && got == 8 && !ring.take(got), "order");
}

void check_margin_and_right() {
    expect(pitlane::margin_both(100, 2, 10, 1, 10, 50) == 200, "money larger");
    expect(pitlane::margin_both(100, 2, 10, 1, 10, 200) == 400, "lot larger");
    expect(pitlane::margin_both(100, 2, 10, 0, 10, 0) == 0, "zero charge");
    expect(pitlane::margin_both(100, 2, 10, 1, 0, 1) == 0, "bad den");
    expect(pitlane::right_value(1, 110, 100) == 10 && pitlane::right_value(2, 110, 100) == 0, "call");
    expect(pitlane::right_value(2, 90, 100) == 10 && pitlane::right_value(3, 90, 100) == -1, "put");
}

}  // namespace

int main() {
    check_window();
    check_pool();
    check_batch();
    check_line_cover_touch();
    check_margin_and_right();
    return g_fails == 0 ? 0 : 1;
}
