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
    check_margin_and_right();
    return g_fails == 0 ? 0 : 1;
}
