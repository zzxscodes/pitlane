#pragma once

#include <cstdint>
#include <cstring>

namespace pitlane {

// Floor of tick to a multiple of width. Negative ticks move away from zero to the block start.
inline int64_t floor_chunk(int64_t tick, int64_t width) noexcept {
    if (width <= 0) return tick;
    int64_t q = tick / width;
    const int64_t r = tick % width;
    if (r != 0 && tick < 0) --q;
    return q * width;
}

// Caller-owned quantity slots. The library does not receive a feed or publish prices.
// A full directory refuses a new block and leaves the old quantities in place.
template <int Ticks, int Blocks>
class ChunkMap {
public:
    static_assert(Ticks >= 8 && Ticks <= 256 && (Ticks % 64) == 0, "ticks");
    static_assert(Blocks >= 1 && Blocks <= 64, "blocks");

    int live() const noexcept { return live_; }

    bool write(int64_t tick, int64_t qty) noexcept {
        if (qty < 0) return false;
        const int64_t base = floor_chunk(tick, Ticks);
        const int at = static_cast<int>(tick - base);
        if (at < 0 || at >= Ticks) return false;
        int block = find(base);
        if (block < 0) {
            if (qty == 0) return true;
            block = alloc(base);
            if (block < 0) return false;
        }
        Chunk& c = chunk_[block];
        const int word = at / 64;
        const uint64_t bit = 1ull << (at % 64);
        if (qty == 0) {
            c.qty[at] = 0;
            c.bits[word] &= ~bit;
            return true;
        }
        c.qty[at] = qty;
        c.bits[word] |= bit;
        return true;
    }

    bool read(int64_t tick, int64_t* qty) const noexcept {
        const int64_t base = floor_chunk(tick, Ticks);
        const int at = static_cast<int>(tick - base);
        if (at < 0 || at >= Ticks) return false;
        const int block = find(base);
        if (block < 0) return false;
        const Chunk& c = chunk_[block];
        const uint64_t bit = 1ull << (at % 64);
        if ((c.bits[at / 64] & bit) == 0) return false;
        if (qty) *qty = c.qty[at];
        return true;
    }

private:
    struct Chunk {
        int64_t base{0};
        int64_t qty[Ticks]{};
        uint64_t bits[Ticks / 64]{};
        uint8_t used{0};
    };

    int find(int64_t base) const noexcept {
        for (int i = 0; i < Blocks; ++i) {
            if (chunk_[i].used && chunk_[i].base == base) return i;
        }
        return -1;
    }

    int alloc(int64_t base) noexcept {
        for (int i = 0; i < Blocks; ++i) {
            if (chunk_[i].used) continue;
            chunk_[i].used = 1;
            chunk_[i].base = base;
            std::memset(chunk_[i].qty, 0, sizeof(chunk_[i].qty));
            std::memset(chunk_[i].bits, 0, sizeof(chunk_[i].bits));
            ++live_;
            return i;
        }
        return -1;
    }

    Chunk chunk_[Blocks]{};
    int live_{0};
};

struct ChainNode {
    uint32_t prev{0xffffffffu};
    uint32_t next{0xffffffffu};
    uint32_t reuse{0};
    int64_t leaves{0};
    uint8_t on{0};
};

struct ChainLevel {
    uint32_t head{0xffffffffu};
    uint32_t tail{0xffffffffu};
    int64_t total{0};
};

struct ChainLoc {
    uint32_t slot{0xffffffffu};
    uint32_t reuse{0};
};

inline bool chain_link(ChainNode* pool, int cap, ChainLevel& level, int slot, int64_t leaves, ChainLoc* loc) noexcept {
    if (!pool || slot < 0 || slot >= cap || leaves <= 0) return false;
    ChainNode& node = pool[slot];
    if (node.on) return false;
    node.prev = level.tail;
    node.next = 0xffffffffu;
    node.leaves = leaves;
    node.on = 1;
    if (level.tail != 0xffffffffu) pool[level.tail].next = static_cast<uint32_t>(slot);
    else level.head = static_cast<uint32_t>(slot);
    level.tail = static_cast<uint32_t>(slot);
    if (level.total > INT64_MAX - leaves) {
        if (node.prev != 0xffffffffu) pool[node.prev].next = 0xffffffffu;
        else level.head = 0xffffffffu;
        level.tail = node.prev;
        node.on = 0;
        node.leaves = 0;
        node.prev = 0xffffffffu;
        node.next = 0xffffffffu;
        return false;
    }
    level.total += leaves;
    if (loc) {
        loc->slot = static_cast<uint32_t>(slot);
        loc->reuse = node.reuse;
    }
    return true;
}

inline bool chain_unlink(ChainNode* pool, int cap, ChainLevel& level, ChainLoc loc) noexcept {
    if (!pool || loc.slot >= static_cast<uint32_t>(cap)) return false;
    ChainNode& node = pool[loc.slot];
    if (!node.on || node.reuse != loc.reuse) return false;
    if (node.prev != 0xffffffffu) pool[node.prev].next = node.next;
    else level.head = node.next;
    if (node.next != 0xffffffffu) pool[node.next].prev = node.prev;
    else level.tail = node.prev;
    if (level.total >= node.leaves) level.total -= node.leaves;
    else level.total = 0;
    node.leaves = 0;
    node.on = 0;
    node.prev = 0xffffffffu;
    node.next = 0xffffffffu;
    ++node.reuse;
    return true;
}

inline bool chain_zero(ChainNode* pool, int cap, ChainLevel& level, ChainLoc loc) noexcept {
    if (!pool || loc.slot >= static_cast<uint32_t>(cap)) return false;
    ChainNode& node = pool[loc.slot];
    if (!node.on || node.reuse != loc.reuse || node.leaves <= 0) return false;
    if (level.total >= node.leaves) level.total -= node.leaves;
    else level.total = 0;
    node.leaves = 0;
    return true;
}

inline int64_t chain_take(ChainNode& node, int64_t want) noexcept {
    if (want <= 0 || node.leaves <= 0) return 0;
    const int64_t qty = node.leaves < want ? node.leaves : want;
    node.leaves -= qty;
    return qty;
}

}  // namespace pitlane
