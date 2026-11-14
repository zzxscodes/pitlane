#pragma once

#include "pitlane/desk/region.hpp"
#include "pitlane/util/hint.hpp"
#include "pitlane/util/simd.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace pitlane {

// Fixed-layout, single-writer, multi-reader snapshot ring. Each slot carries a
// sequence lock, so a reader copies a whole record without taking a lock and
// retries if the writer reused the slot during the copy.
//
// The header is exactly 64 bytes and the cells start at offset 64. A reader in
// Python, Rust, or another process maps the same file, reads magic/version/
// slot_bytes/stride/cap, then reads cell i at 64 + i * stride. It never needs
// the C++ type, only the field layout of the caller's T.

inline constexpr uint32_t kSnapVersion = 1;

struct alignas(64) SnapHead {
    char magic[8]{};
    uint32_t version{0};
    uint32_t slot_bytes{0}; // sizeof(T), the payload only
    uint32_t stride{0};     // bytes between adjacent cells, including the seq word
    uint32_t cap{0};        // power of two
    std::atomic<uint64_t> published{0};
    uint32_t writer_pid{0};
    uint32_t pad0{0};
    uint8_t pad_[24]{};
};

static_assert(sizeof(SnapHead) == 64, "snap head");
static_assert(alignof(SnapHead) == 64, "snap head alignment");

template <class T, uint32_t Cap>
class SnapRing {
public:
    static_assert((Cap & (Cap - 1u)) == 0u && Cap >= 2u, "snap cap");
    static_assert(std::is_trivially_copyable<T>::value, "snap payload");

    struct alignas(64) Cell {
        std::atomic<uint64_t> seq{0};
        T body{};
    };

    static constexpr uint32_t slot_stride() noexcept { return static_cast<uint32_t>(sizeof(Cell)); }

    void format() noexcept {
        new (this) SnapRing();
        static constexpr char kMagic[8] = {'P', 'I', 'T', 'S', 'N', 'A', 'P', '1'};
        std::memcpy(head_.magic, kMagic, sizeof kMagic);
        head_.version = kSnapVersion;
        head_.slot_bytes = static_cast<uint32_t>(sizeof(T));
        head_.stride = static_cast<uint32_t>(sizeof(Cell));
        head_.cap = Cap;
    }

    uint64_t published() const noexcept { return head_.published.load(std::memory_order_acquire); }
    uint64_t capacity() const noexcept { return Cap; }
    const SnapHead& head() const noexcept { return head_; }
    SnapHead& head() noexcept { return head_; }

    // Appends one record. The sequence word goes odd while the body is written
    // and even again before the published count advances. A reader that sees a
    // reused slot retries; it never observes a half-written body as valid.
    PITLANE_HOT bool publish(const T& item, uint64_t* pos_out = nullptr) noexcept {
        const uint64_t pos = head_.published.load(std::memory_order_relaxed);
        Cell& cell = cell_[pos & (Cap - 1u)];
        const uint64_t seq = cell.seq.load(std::memory_order_relaxed);
        cell.seq.store(seq + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        copy_bytes(&cell.body, &item, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        cell.seq.store(seq + 2u, std::memory_order_release);
        head_.published.store(pos + 1u, std::memory_order_release);
        if (pos_out) *pos_out = pos;
        return true;
    }

    // Copies a published record. Returns false when the position is not
    // published yet or the slot churned through the whole retry budget.
    PITLANE_HOT bool read(uint64_t pos, T& out) const noexcept {
        const uint64_t pub = head_.published.load(std::memory_order_acquire);
        if (unlikely(pos >= pub)) return false;
        // The writer may have wrapped past this slot; a stale position must
        // not silently return a newer record.
        if (unlikely(pub - pos > Cap)) return false;
        const Cell& cell = cell_[pos & (Cap - 1u)];
        for (int retry = 0; retry < 64; ++retry) {
            const uint64_t s1 = cell.seq.load(std::memory_order_acquire);
            if (unlikely((s1 & 1u) != 0u)) {
                cpu_relax();
                continue;
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            copy_bytes(&out, &cell.body, sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t s2 = cell.seq.load(std::memory_order_acquire);
            if (likely(s1 == s2)) {
                const uint64_t after = head_.published.load(std::memory_order_acquire);
                return after - pos <= Cap;
            }
        }
        return false;
    }

    PITLANE_INLINE bool latest(T& out) const noexcept {
        const uint64_t n = published();
        if (unlikely(n == 0)) return false;
        return read(n - 1u, out);
    }

private:
    SnapHead head_{};
    Cell cell_[Cap]{};
};

template <class T, uint32_t Cap>
inline std::size_t snap_bytes() noexcept {
    return sizeof(SnapRing<T, Cap>);
}

template <class T, uint32_t Cap>
inline SnapRing<T, Cap>* snap_view(MapInfo* io, int create_if_empty) noexcept {
    if (!io || !io->addr || io->bytes < snap_bytes<T, Cap>()) return nullptr;
    auto* ring = static_cast<SnapRing<T, Cap>*>(io->addr);
    static constexpr char kMagic[8] = {'P', 'I', 'T', 'S', 'N', 'A', 'P', '1'};
    if (std::memcmp(ring->head().magic, kMagic, sizeof kMagic) == 0) {
        if (ring->head().version != kSnapVersion || ring->head().slot_bytes != sizeof(T) ||
            ring->head().stride != SnapRing<T, Cap>::slot_stride() ||
            ring->head().cap != Cap) {
            return nullptr;
        }
        return ring;
    }
    bool blank = true;
    for (char c : ring->head().magic) {
        if (c != 0) blank = false;
    }
    if (!blank || !create_if_empty) return nullptr;
    ring->format();
    return ring;
}

template <class T, uint32_t Cap>
inline int snap_open(const char* path, MapInfo* out, int create, MapOpt opt = {}) noexcept {
    if (!path || !out) return -1;
    const std::size_t bytes = snap_bytes<T, Cap>();
    return create ? map_create(path, bytes, out, opt) : map_attach(path, bytes, out, opt);
}

} // namespace pitlane
