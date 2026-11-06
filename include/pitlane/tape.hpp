#pragma once

#include <atomic>
#include <cstdint>

namespace pitlane {

// Diagnostic records on storage the caller owns.
// A disabled gate does not write. A full high-rate tape overwrites the oldest record.
// A spike that does not fit increments a drop count and does not block the caller.

struct alignas(64) TraceGate {
    std::atomic<uint64_t> word{0};
    unsigned char pad_[64 - sizeof(std::atomic<uint64_t>)]{};

    void arm(uint16_t event, bool on) noexcept {
        uint64_t w = on ? 1ull : 0ull;
        w |= static_cast<uint64_t>(event) << 48;
        word.store(w, std::memory_order_release);
    }

    bool on() const noexcept { return (word.load(std::memory_order_acquire) & 1ull) != 0; }

    uint16_t event() const noexcept {
        return static_cast<uint16_t>(word.load(std::memory_order_acquire) >> 48);
    }
};

static_assert(sizeof(TraceGate) == 64, "trace gate occupies one line");

template <uint32_t Cap>
class CoreTape {
public:
    static_assert((Cap & (Cap - 1u)) == 0u, "power of two");
    static_assert(Cap >= 16u && Cap <= 4096u, "records");

    struct Rec {
        uint64_t tsc{0};
        uint32_t id{0};
        uint32_t a{0};
        int32_t b{0};
    };

    bool note(const TraceGate& gate, uint64_t tsc, uint32_t id, uint32_t a, int32_t b) noexcept {
        const uint64_t w = gate.word.load(std::memory_order_acquire);
        if ((w & 1ull) == 0) return false;
        const uint32_t h = head_.load(std::memory_order_relaxed);
        Rec& rec = rec_[h & (Cap - 1u)];
        rec.tsc = tsc;
        rec.id = id;
        rec.a = a;
        rec.b = b;
        head_.store(h + 1u, std::memory_order_release);
        return true;
    }

    uint32_t count() const noexcept {
        const uint32_t h = head_.load(std::memory_order_acquire);
        return h > Cap ? Cap : h;
    }

    bool at(uint32_t index, Rec& out) const noexcept {
        const uint32_t h = head_.load(std::memory_order_acquire);
        if (index >= h || (h > Cap && index < h - Cap)) return false;
        out = rec_[index & (Cap - 1u)];
        return true;
    }

private:
    Rec rec_[Cap]{};
    alignas(64) std::atomic<uint32_t> head_{0};
};

template <uint32_t Seg, uint32_t N>
class SegTape {
public:
    static_assert((Seg & (Seg - 1u)) == 0u && Seg >= 2u && Seg <= 16u, "segments");
    static_assert(N >= 4u && N <= 1024u, "records");

    struct Rec {
        uint64_t t0{0};
        uint64_t t1{0};
        uint32_t id{0};
    };

    bool push(uint64_t t0, uint64_t t1, uint32_t id) noexcept {
        seg_[cur_][at_] = Rec{t0, t1, id};
        if (++at_ < N) return true;
        const uint32_t nseal = sealed_.load(std::memory_order_relaxed);
        const uint32_t ndone = done_.load(std::memory_order_acquire);
        if (nseal - ndone >= Seg - 1u) {
            at_ = 0;
            ++overrun_;
            return false;
        }
        sealed_.store(nseal + 1u, std::memory_order_release);
        cur_ = (cur_ + 1u) & (Seg - 1u);
        at_ = 0;
        return true;
    }

    bool take(uint32_t& index) const noexcept {
        const uint32_t ndone = done_.load(std::memory_order_relaxed);
        const uint32_t nseal = sealed_.load(std::memory_order_acquire);
        if (ndone == nseal) return false;
        index = ndone & (Seg - 1u);
        return true;
    }

    const Rec* segment(uint32_t index) const noexcept { return seg_[index & (Seg - 1u)]; }

    void give() noexcept {
        const uint32_t ndone = done_.load(std::memory_order_relaxed);
        done_.store(ndone + 1u, std::memory_order_release);
    }

    uint32_t overrun() const noexcept { return overrun_; }

private:
    Rec seg_[Seg][N]{};
    uint32_t at_{0};
    uint32_t cur_{0};
    uint32_t overrun_{0};
    std::atomic<uint32_t> sealed_{0};
    alignas(64) std::atomic<uint32_t> done_{0};
};

template <uint32_t Cap>
class SpikeRing {
public:
    static_assert((Cap & (Cap - 1u)) == 0u && Cap >= 8u && Cap <= 4096u, "notes");

    struct Note {
        uint64_t t0{0};
        uint64_t t1{0};
        uint32_t seq{0};
        uint32_t cpu{0};
    };

    bool signal(uint64_t t0, uint64_t t1, uint32_t seq, uint32_t cpu, uint64_t limit) noexcept {
        if (t1 < t0 || t1 - t0 <= limit) return false;
        const uint32_t wr = w_.load(std::memory_order_relaxed);
        const uint32_t rd = r_.load(std::memory_order_acquire);
        if (wr - rd >= Cap) {
            ++dropped_;
            return false;
        }
        slot_[wr & (Cap - 1u)] = Note{t0, t1, seq, cpu};
        w_.store(wr + 1u, std::memory_order_release);
        return true;
    }

    bool pop(Note& out) noexcept {
        const uint32_t rd = r_.load(std::memory_order_relaxed);
        const uint32_t wr = w_.load(std::memory_order_acquire);
        if (rd == wr) return false;
        out = slot_[rd & (Cap - 1u)];
        r_.store(rd + 1u, std::memory_order_release);
        return true;
    }

    uint32_t dropped() const noexcept { return dropped_; }

private:
    Note slot_[Cap]{};
    std::atomic<uint32_t> w_{0};
    uint32_t dropped_{0};
    alignas(64) std::atomic<uint32_t> r_{0};
};

}  // namespace pitlane
