// Hot-path allocation guard.
//
// "No heap on the hot path" means: after startup is over, a call must not
// allocate or free memory, and must not open or close a descriptor. This area
// replaces the global allocator so the counters are exact, performs every
// startup step first (mmap, prefault, calibration, table fill), then runs the
// 2.22 hot paths and asserts the counters did not move.

#include "pitlane/pitlane.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <new>
#include <unistd.h>

static std::atomic<uint64_t> g_alloc{0};
static std::atomic<uint64_t> g_free{0};

static void* counted_alloc(std::size_t n) noexcept {
    void* p = std::malloc(n == 0 ? 1u : n);
    if (p) g_alloc.fetch_add(1u, std::memory_order_relaxed);
    return p;
}

static void* counted_alloc_aligned(std::size_t n, std::size_t align) noexcept {
    if (align < sizeof(void*)) align = sizeof(void*);
    std::size_t bytes = n == 0 ? 1u : n;
    const std::size_t rem = bytes % align;
    if (rem != 0) bytes += align - rem;
    void* p = std::aligned_alloc(align, bytes);
    if (p) g_alloc.fetch_add(1u, std::memory_order_relaxed);
    return p;
}

static void* counted_free(void* p) noexcept {
    if (p) {
        g_free.fetch_add(1u, std::memory_order_relaxed);
        std::free(p);
    }
    return nullptr;
}

void* operator new(std::size_t n) {
    void* p = counted_alloc(n);
    if (!p) std::abort();
    return p;
}

void* operator new[](std::size_t n) { return ::operator new(n); }

void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }

void* operator new(std::size_t n, std::align_val_t a) {
    void* p = counted_alloc_aligned(n, static_cast<std::size_t>(a));
    if (!p) std::abort();
    return p;
}

void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }

void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_alloc_aligned(n, static_cast<std::size_t>(a));
}

void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_alloc_aligned(n, static_cast<std::size_t>(a));
}

void operator delete(void* p) noexcept { counted_free(p); }

void operator delete[](void* p) noexcept { counted_free(p); }

void operator delete(void* p, std::size_t) noexcept { counted_free(p); }

void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }

void operator delete(void* p, const std::nothrow_t&) noexcept { counted_free(p); }

void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_free(p); }

void operator delete(void* p, std::align_val_t) noexcept { counted_free(p); }

void operator delete[](void* p, std::align_val_t) noexcept { counted_free(p); }

void operator delete(void* p, std::size_t, std::align_val_t) noexcept { counted_free(p); }

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { counted_free(p); }

static int g_fails = 0;

static void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

// Counter snapshot taken after startup and before a hot-path section.
struct Mark {
    uint64_t alloc;
    uint64_t free;
};

static Mark mark() noexcept {
    return Mark{g_alloc.load(std::memory_order_relaxed), g_free.load(std::memory_order_relaxed)};
}

static void expect_quiet(const Mark& m, const char* what) {
    const uint64_t a = g_alloc.load(std::memory_order_relaxed) - m.alloc;
    const uint64_t f = g_free.load(std::memory_order_relaxed) - m.free;
    if (a == 0 && f == 0) return;
    std::fprintf(stderr, "fail: %s touched the heap (%llu alloc, %llu free)\n", what,
                 static_cast<unsigned long long>(a), static_cast<unsigned long long>(f));
    ++g_fails;
}

// The guard is only meaningful if the counters really move. This control calls
// the replaced entry points directly so the pair cannot be elided.
static void check_counter_live() {
    const Mark m = mark();
    void* p = ::operator new(sizeof(int));
    const uint64_t allocs = g_alloc.load(std::memory_order_relaxed) - m.alloc;
    ::operator delete(p);
    const uint64_t frees = g_free.load(std::memory_order_relaxed) - m.free;
    expect(allocs == 1 && frees == 1, "counter live");
}

static int count_fds() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (!dir) return -1;
    int n = 0;
    while (::readdir(dir) != nullptr) ++n;
    ::closedir(dir);
    return n;
}

struct Item {
    uint64_t a{0};
    uint64_t b{0};
};

static void check_wal() {
    char path[64]{};
    std::snprintf(path, sizeof path, "/tmp/pitlane_alloc_wal_%ld.bin", static_cast<long>(::getpid()));
    pitlane::WalView wal{};
    expect(pitlane::wal_open(path, 1024, &wal, 1) == 0, "wal open");

    const Mark m = mark();
    Item item{};
    bool ok = true;
    for (int i = 0; i < 1000; ++i) {
        item.a = static_cast<uint64_t>(i);
        item.b = item.a + 1u;
        ok = pitlane::wal_append_item(wal, pitlane::WalKind::Intent, item, item.a) && ok;
    }
    const uint64_t tail = pitlane::wal_tail(wal);
    expect_quiet(m, "wal_append_item");
    expect(ok && tail == 1000, "wal append");

    uint64_t replayed = 0;
    const int n = pitlane::wal_recover(wal, [&](const pitlane::WalRec&) { ++replayed; });
    expect(n == 1000 && replayed == 1000, "wal recover");
    pitlane::wal_close(&wal);
    ::unlink(path);
}

static constexpr uint32_t kSnapCap = 16;

struct SnapItem {
    uint64_t seq{0};
    int64_t px{0};
    int64_t qty{0};
    uint32_t tag{0};
    uint32_t pad{0};
};

alignas(64) static unsigned char g_snap_buf[sizeof(pitlane::SnapRing<SnapItem, kSnapCap>)];

static void check_snap() {
    auto* ring = new (g_snap_buf) pitlane::SnapRing<SnapItem, kSnapCap>();
    ring->format();

    const Mark m = mark();
    SnapItem item{};
    SnapItem out{};
    bool ok = true;
    for (int i = 0; i < 4096; ++i) {
        item.seq = static_cast<uint64_t>(i);
        item.px = 100;
        ok = ring->publish(item) && ok;
        ok = ring->read(static_cast<uint64_t>(i), out) && ok;
    }
    expect_quiet(m, "snap publish/read");
    expect(ok && out.seq == 4095, "snap state");
}

static void check_tsc() {
    pitlane::TscClock clock;
    if (!clock.calibrate(1000)) {
        expect(false, "tsc calibrate");
        return;
    }
    pitlane::UsSlot slot{};

    const Mark m = mark();
    uint64_t ns = 0;
    int64_t us = 0;
    bool ok = true;
    for (int i = 0; i < 100000; ++i) {
        ok = clock.now_ns(ns) && ok;
        ok = clock.now_us(us) && ok;
        ok = clock.publish(slot) && ok;
    }
    expect_quiet(m, "tsc now/publish");
    expect(ok && ns > 0 && us > 0 && pitlane::us_load(slot) == us, "tsc state");
}

static void check_tail() {
    pitlane::TailBoard board{};
    pitlane::tail_arm(board, 1000000, 0xffu);
    pitlane::TailTape<64> tape;

    const Mark m = mark();
    for (int i = 0; i < 4096; ++i) {
        (void)pitlane::tail_note(board, pitlane::TailLane::Intent, static_cast<uint64_t>(i % 5000));
        (void)pitlane::tail_gap(board, pitlane::TailLane::Intent, 10);
        tape.note(0, 1, static_cast<uint64_t>(i));
        if (i == 2048) (void)tape.freeze();
    }
    pitlane::LatReport rep{};
    const bool reported = pitlane::tail_report(board, pitlane::TailLane::Intent, rep);
    expect_quiet(m, "tail_note");
    expect(reported && board.lane[0].notes == 4096 && board.lane[0].overruns == 0, "tail state");
    expect(tape.frozen() && tape.count() == 64, "tail tape");
}

static void check_shadow() {
    pitlane::ShadowBook book;
    const int row = book.open("twap");
    expect(row >= 0, "shadow open");

    const Mark m = mark();
    pitlane::ShadowPair p{};
    p.algo = 1;
    p.side = 1;
    p.tick = 1;
    p.real_qty = 1;
    p.sim_qty = 1;
    p.sim_px = 100;
    for (int i = 0; i < 4096; ++i) {
        p.tag = static_cast<uint32_t>(i);
        p.real_px = 100 + (i % 3);
        book.note(row, p);
    }
    pitlane::ShadowReport rep{};
    const bool reported = book.report(row, rep);
    expect_quiet(m, "shadow note");
    expect(reported && rep.pairs == 4096, "shadow state");
}

static void check_mbo() {
    static pitlane::MboBook book;
    book.clear();

    const Mark m = mark();
    bool ok = true;
    for (int i = 0; i < 2000; ++i) {
        pitlane::MboMsg msg{};
        msg.id = static_cast<uint64_t>(i + 1);
        msg.px = 100 + (i % 5);
        msg.qty = 10;
        msg.side = static_cast<uint8_t>(pitlane::MboSide::Bid);
        msg.act = static_cast<uint8_t>(pitlane::MboAct::Add);
        ok = book.apply(msg) && ok;
        msg.act = static_cast<uint8_t>(pitlane::MboAct::Modify);
        msg.qty = 20;
        ok = book.apply(msg) && ok;
        msg.act = static_cast<uint8_t>(pitlane::MboAct::Trade);
        msg.qty = 5;
        ok = book.apply(msg) && ok;
        msg.act = static_cast<uint8_t>(pitlane::MboAct::Cancel);
        ok = book.apply(msg) && ok;
    }
    pitlane::MboLevel bid{};
    pitlane::MboLevel ask{};
    (void)book.best(bid, ask);
    const bool inv = book.invariant();
    expect_quiet(m, "mbo apply");
    expect(ok && inv && book.live() == 0, "mbo state");
}

static void check_ring() {
    static pitlane::FanRing<Item, 64> ring;
    uint64_t cur = 0;

    const Mark m = mark();
    uint64_t sink = 0;
    bool ok = true;
    for (int i = 0; i < 4096; ++i) {
        Item item{static_cast<uint64_t>(i), 0};
        ok = ring.publish(item, 1ull << 5) && ok;
        pitlane::ring_take(ring, cur, 5, false, [&](const Item& v) { sink += v.a; });
    }
    expect_quiet(m, "fanring publish/take");
    expect(ok && cur == 4096 && sink == 4096ull * 4095ull / 2ull, "fanring state");
}

static void check_replay() {
    pitlane::ReplayTape<Item, 64> tape;
    for (int i = 0; i < 64; ++i) {
        Item item{static_cast<uint64_t>(i), 0};
        expect(tape.push(static_cast<uint64_t>(i), item, static_cast<uint64_t>(i)), "tape push");
    }
    static pitlane::FanRing<Item, 64> ring;
    pitlane::FaultPlan plan{};

    const Mark m = mark();
    uint64_t sink = 0;
    bool ok = true;
    for (int r = 0; r < 500; ++r) {
        const pitlane::ReplayOut out =
            pitlane::replay_ring(ring, tape, plan, [&](const pitlane::ReplayNote<Item>& n) { sink += n.item.a; });
        ok = (out.published == 64 && out.blocked == 0) && ok;
    }
    expect_quiet(m, "replay_ring");
    expect(ok && sink == 500ull * 64ull * 63ull / 2ull, "replay state");
}

int main() {
    check_counter_live();
    const int fds_before = count_fds();
    check_wal();
    check_snap();
    check_tsc();
    check_tail();
    check_shadow();
    check_mbo();
    check_ring();
    check_replay();
    expect(count_fds() == fds_before, "descriptor count unchanged");

    if (g_fails) {
        std::fprintf(stderr, "%d alloc check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
