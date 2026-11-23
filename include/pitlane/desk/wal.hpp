#pragma once

#include "pitlane/desk/region.hpp"
#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <sys/mman.h>
#include <type_traits>

namespace pitlane {

// Fixed-size write-ahead log on a file-backed mapping. One writer appends,
// readers replay. The tail is the commit marker: a record becomes visible only
// after its body and checksum are written. Recovery stops at the first bad
// record and moves the tail back, so a torn write never becomes state.
//
// The log stores caller-defined payloads. A trading process typically appends
// the Intent, Report, cash, and position records it already owns; a recovery
// callback applies them to a fresh desk. The library does not choose what a
// record means.

inline constexpr uint32_t kWalVersion = 1;
inline constexpr uint32_t kWalPayload = 128;
inline constexpr uint32_t kWalCapMax = 1u << 20;

enum class WalKind : uint16_t {
    Mark = 1,
    Intent = 2,
    Report = 3,
    Cash = 4,
    Hold = 5,
    Algo = 6,
    Control = 7,
    Signal = 8,
    Quote = 9
};

struct alignas(64) WalHead {
    char magic[8]{};
    uint32_t version{0};
    uint32_t payload{0};
    uint32_t cap{0};
    uint32_t pad0{0};
    std::atomic<uint64_t> tail{0};
    std::atomic<uint64_t> syncs{0};
    uint8_t pad_[24]{};
};

static_assert(sizeof(WalHead) == 64, "wal head");

struct alignas(64) WalRec {
    uint64_t seq{0};
    uint64_t stamp_us{0};
    uint32_t crc{0};
    uint16_t kind{0};
    uint16_t flags{0};
    uint32_t len{0};
    uint8_t data[kWalPayload]{};
};

static_assert(sizeof(WalRec) == 192, "wal record");

struct WalView {
    MapInfo map{};
    WalHead* head{nullptr};
    WalRec* rec{nullptr};
    uint32_t cap{0};
};

inline std::size_t wal_bytes(uint32_t cap) noexcept {
    return sizeof(WalHead) + static_cast<std::size_t>(cap) * sizeof(WalRec);
}

inline uint32_t wal_crc_update(uint32_t crc, const void* src, std::size_t n) noexcept {
    const auto* p = static_cast<const uint8_t*>(src);
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return crc;
}

// CRC32C over the record fields, not over the struct padding.
inline uint32_t wal_crc_record(const WalRec& r) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    crc = wal_crc_update(crc, &r.seq, sizeof r.seq);
    crc = wal_crc_update(crc, &r.stamp_us, sizeof r.stamp_us);
    crc = wal_crc_update(crc, &r.kind, sizeof r.kind);
    crc = wal_crc_update(crc, &r.len, sizeof r.len);
    const uint32_t len = r.len <= kWalPayload ? r.len : kWalPayload;
    crc = wal_crc_update(crc, r.data, len);
    return ~crc;
}

inline int wal_open(const char* path, uint32_t cap, WalView* out, int create, MapOpt opt = {}) noexcept {
    // Header-only helper; the mapping is released through map_release().
    if (!path || !out || cap < 2 || cap > kWalCapMax) return -1;
    out->map = MapInfo{};
    out->head = nullptr;
    out->rec = nullptr;
    out->cap = 0;

    // The WAL needs ordinary file-backed pages so msync() is meaningful, and
    // the pages are prefaulted at open time so an append after startup does not
    // take a page fault or allocate a kernel page on the measured path.
    opt.hugepage = 0;
    opt.prefault = 1;
    const std::size_t bytes = wal_bytes(cap);
    if (bytes < sizeof(WalHead)) return -1;
    const int rc = create ? map_create(path, bytes, &out->map, opt) : map_attach(path, bytes, &out->map, opt);
    if (rc != 0 || !out->map.addr || out->map.bytes < bytes) {
        map_release(&out->map);
        return -1;
    }

    auto* head = static_cast<WalHead*>(out->map.addr);
    auto* rec = reinterpret_cast<WalRec*>(static_cast<uint8_t*>(out->map.addr) + sizeof(WalHead));
    bool blank = true;
    for (char c : head->magic) {
        if (c != 0) blank = false;
    }
    if (blank) {
        if (!create) {
            map_release(&out->map);
            return -1;
        }
        new (head) WalHead();
        static constexpr char kMagic[8] = {'P', 'I', 'T', 'W', 'A', 'L', '1', 0};
        std::memcpy(head->magic, kMagic, sizeof kMagic);
        head->version = kWalVersion;
        head->payload = kWalPayload;
        head->cap = cap;
        for (uint32_t i = 0; i < cap; ++i) new (&rec[i]) WalRec();
    } else {
        static constexpr char kMagic[8] = {'P', 'I', 'T', 'W', 'A', 'L', '1', 0};
        if (std::memcmp(head->magic, kMagic, sizeof kMagic) != 0 || head->version != kWalVersion ||
            head->payload != kWalPayload || head->cap != cap) {
            map_release(&out->map);
            return -1;
        }
    }
    out->head = head;
    out->rec = rec;
    out->cap = cap;
    return 0;
}

inline void wal_close(WalView* io) noexcept {
    if (!io) return;
    map_release(&io->map);
    io->head = nullptr;
    io->rec = nullptr;
    io->cap = 0;
}

PITLANE_INLINE uint64_t wal_tail(const WalView& view) noexcept {
    return view.head ? view.head->tail.load(std::memory_order_acquire) : 0;
}

// Appends one record. Fails closed when the log is full, len exceeds the slot,
// or the writer is missing. seq_out receives the 1-based record number.
PITLANE_HOT inline bool wal_append(WalView& view, WalKind kind, const void* data, uint32_t len, uint64_t stamp_us,
                                   uint64_t* seq_out = nullptr) noexcept {
    if (unlikely(!view.head || !view.rec || len > kWalPayload || (len > 0 && !data))) return false;
    const uint64_t tail = view.head->tail.load(std::memory_order_relaxed);
    if (unlikely(tail >= view.cap)) return false;
    WalRec& rec = view.rec[tail];
    rec.seq = tail + 1u;
    rec.stamp_us = stamp_us;
    rec.kind = static_cast<uint16_t>(kind);
    rec.flags = 0;
    rec.len = len;
    if (len > 0) std::memcpy(rec.data, data, len);
    rec.crc = wal_crc_record(rec);
    std::atomic_thread_fence(std::memory_order_release);
    view.head->tail.store(tail + 1u, std::memory_order_release);
    if (seq_out) *seq_out = tail + 1u;
    return true;
}

template <class T>
PITLANE_INLINE bool wal_append_item(WalView& view, WalKind kind, const T& item, uint64_t stamp_us,
                                    uint64_t* seq_out = nullptr) noexcept {
    static_assert(std::is_trivially_copyable<T>::value, "wal payload");
    static_assert(sizeof(T) <= kWalPayload, "wal payload fits one record");
    return wal_append(view, kind, &item, static_cast<uint32_t>(sizeof(T)), stamp_us, seq_out);
}

// Flushes the mapping. The append path does not call this; a caller decides
// when a group commit is due. A crash before sync still replays whatever the
// kernel wrote, and CRC rejects a torn tail.
inline bool wal_sync(WalView& view) noexcept {
    if (!view.map.addr || view.map.bytes == 0) return false;
    if (::msync(view.map.addr, view.map.bytes, MS_SYNC) != 0) return false;
    if (view.head) view.head->syncs.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Replays committed records in order and truncates the tail at the first
// record whose sequence, kind, length, or CRC does not match. Returns the
// number of records replayed, or -1 when the view is not open.
template <class Fn>
int wal_recover(WalView& view, Fn&& fn) noexcept {
    if (!view.head || !view.rec) return -1;
    const uint64_t raw_tail = view.head->tail.load(std::memory_order_acquire);
    const uint64_t tail = raw_tail > view.cap ? 0 : raw_tail;
    uint64_t good = 0;
    for (uint64_t i = 0; i < tail; ++i) {
        const WalRec& rec = view.rec[i];
        if (unlikely(rec.seq != i + 1u || rec.len > kWalPayload || rec.kind == 0 ||
                     rec.crc != wal_crc_record(rec))) {
            break;
        }
        fn(rec);
        good = i + 1u;
    }
    if (raw_tail != good) view.head->tail.store(good, std::memory_order_release);
    return static_cast<int>(good);
}

} // namespace pitlane
