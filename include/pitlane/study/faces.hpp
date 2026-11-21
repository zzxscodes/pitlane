#pragma once

#include "pitlane/util/hint.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace pitlane {

// Per-instrument named rings for factors, samples, and unstructured signals.
// A producer publishes one cell under a short name; every
// subscriber keeps its own cursor and drains a batch. Storage is fixed, the
// write path is allocation-free, and a key is resolved once at subscription.

struct NamedCell {
    int64_t reading{0};
    uint8_t ready{0};
    uint8_t pad[7]{};
};
static_assert(sizeof(NamedCell) == 16, "one flag and one value in 16 bytes");
static_assert(std::is_trivially_copyable<NamedCell>::value, "NamedCell");

// Unstructured signal: calculation, production, and send stamps stay apart so
// a consumer can tell compute latency from transport latency. corp_ppm is the
// corporate-action scale in parts per million (1000000 = unchanged).
struct OpenSignal {
    char code[48]{};
    char freq[16]{};
    int64_t computed_ns{0};
    int64_t produced_ns{0};
    int64_t sent_ns{0};
    char label[24]{};
    int32_t corp_ppm{1000000};
    uint8_t corp_pad[4]{};
    int64_t reading{0};
    uint8_t ready{0};
    uint8_t pad[7]{};
};
static_assert(std::is_trivially_copyable<OpenSignal>::value, "OpenSignal");

// One name owns one overwrite ring. head is the total number of publishes and
// never rewinds; cell[head & mask] is the newest. A reader that falls more than
// Depth behind loses the overwritten prefix and is told how many cells.
template <class Cell, uint32_t Depth>
class NamedRing {
public:
    static_assert(Depth >= 2 && (Depth & (Depth - 1)) == 0, "Depth must be a power of two");
    static_assert(std::is_trivially_copyable<Cell>::value, "Cell must be trivially copyable");

    static constexpr uint32_t kMask = Depth - 1;

    bool open(const char* key) noexcept {
        if (!key || !key[0]) return false;
        std::size_t n = 0;
        for (; key[n] && n < sizeof key_ - 1; ++n) key_[n] = key[n];
        if (key[n]) return false;
        key_[n] = 0;
        used_ = 1;
        head_.store(0, std::memory_order_relaxed);
        return true;
    }

    bool used() const noexcept { return used_ != 0; }
    const char* key() const noexcept { return key_; }

    bool same_key(const char* key) const noexcept {
        return used_ != 0 && key && std::strcmp(key_, key) == 0;
    }

    PITLANE_HOT bool push(const Cell& cell) noexcept {
        const uint64_t pos = head_.load(std::memory_order_relaxed);
        cell_[pos & kMask] = cell;
        head_.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Copies the next unread cell for cursor. Sets lost to the number of cells
    // the reader missed if it fell more than Depth behind. Returns false when
    // the ring is empty for that cursor.
    PITLANE_HOT bool take(uint64_t& cursor, Cell& out, uint64_t* lost = nullptr) const noexcept {
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (cursor >= head) return false;
        if (head - cursor > Depth) {
            if (lost) *lost = head - Depth - cursor;
            cursor = head - Depth;
        }
        out = cell_[cursor & kMask];
        ++cursor;
        return true;
    }

    bool latest(Cell& out) const noexcept {
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (head == 0) return false;
        out = cell_[(head - 1) & kMask];
        return true;
    }

    uint64_t head() const noexcept { return head_.load(std::memory_order_acquire); }
    uint64_t unread(uint64_t cursor) const noexcept {
        const uint64_t head = head_.load(std::memory_order_acquire);
        return head > cursor ? head - cursor : 0;
    }

private:
    char key_[32]{};
    uint8_t used_{0};
    uint8_t pad_[7]{};
    Cell cell_[Depth]{};
    alignas(kCacheLine) std::atomic<uint64_t> head_{0};
};

template <class Cell>
struct FaceRow {
    char key[32]{};
    Cell cell{};
};
static_assert(std::is_trivially_copyable<FaceRow<NamedCell>>::value, "FaceRow");

// Drains every named ring once, round-robin by ring order, and updates one
// cursor per ring. Returns the number of rows written.
template <class Cell, uint32_t Depth>
PITLANE_HOT int drain_named(NamedRing<Cell, Depth>* rings, int names, uint64_t* cursors,
                            FaceRow<Cell>* out, int cap) noexcept {
    if (!rings || !cursors || !out || names <= 0 || cap <= 0) return 0;
    int written = 0;
    for (int i = 0; i < names && written < cap; ++i) {
        if (!rings[i].used()) continue;
        Cell cell{};
        while (written < cap && rings[i].take(cursors[i], cell)) {
            std::memcpy(out[written].key, rings[i].key(), sizeof out[written].key);
            out[written].cell = cell;
            ++written;
        }
    }
    return written;
}

// Live face set bound from a pool at startup. The pointers stay valid for the
// life of the pool; no instrument stores a ring inline.
template <uint32_t FactorDepth = 256, uint32_t SampleDepth = 512, uint32_t OpenDepth = 512>
struct InstrumentFaces {
    NamedRing<NamedCell, FactorDepth>* factor{nullptr};
    NamedRing<NamedCell, SampleDepth>* sample{nullptr};
    NamedRing<OpenSignal, OpenDepth>* open_sig{nullptr};
    uint32_t index{0};
};

enum class FaceTag : uint8_t { Factor = 0, Sample = 1, Open = 2 };
enum class FaceSub : uint8_t { Request = 0, Pulse = 1, Open = 2 };

// Startup pool. Sizes are compile-time and callers choose Pools; the default
// name budgets are 20 factors, 100 samples, and 10 open signals.
template <int Pools, int FactorNames = 20, int SampleNames = 100, int OpenNames = 10,
          uint32_t FactorDepth = 256, uint32_t SampleDepth = 512, uint32_t OpenDepth = 512>
class FacePool {
public:
    static_assert(Pools >= 1 && FactorNames >= 1 && SampleNames >= 1 && OpenNames >= 1, "face sizes");

    bool bind(InstrumentFaces<FactorDepth, SampleDepth, OpenDepth>& out) noexcept {
        const uint32_t i = used_.fetch_add(1, std::memory_order_relaxed);
        if (i >= static_cast<uint32_t>(Pools)) return false;
        out.factor = factor_[i];
        out.sample = sample_[i];
        out.open_sig = open_[i];
        out.index = i;
        return true;
    }

    uint32_t used() const noexcept { return used_.load(std::memory_order_relaxed); }

private:
    alignas(kCacheLine) NamedRing<NamedCell, FactorDepth> factor_[Pools][FactorNames]{};
    alignas(kCacheLine) NamedRing<NamedCell, SampleDepth> sample_[Pools][SampleNames]{};
    alignas(kCacheLine) NamedRing<OpenSignal, OpenDepth> open_[Pools][OpenNames]{};
    std::atomic<uint32_t> used_{0};
};

}  // namespace pitlane
