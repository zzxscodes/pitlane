// Live strategy instance registry. Specs, states and rows are owned by the
// trading thread; the HTTP thread only pushes commands into an SPSC ring and
// reads published rows through a per-slot seqlock. No locks, no heap.

#pragma once

#include "pitlane/util/sendjoin.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

enum StratState : uint8_t {
    kStratIdle = 0,
    kStratRunning = 1,
    kStratPaused = 2,
    kStratStopped = 3,
    kStratFailed = 4
};

inline const char* strat_state_name(uint8_t s) noexcept {
    switch (s) {
        case kStratRunning: return "running";
        case kStratPaused: return "paused";
        case kStratStopped: return "stopped";
        case kStratFailed: return "failed";
        default: return "idle";
    }
}

// 因子类策略店 1..8，配对价差是 9：instrument 为 A 腿，instrument_b 为 B 腿。
enum : uint8_t { kStratPairKind = 9 };

enum : uint8_t {
    kStratCmdAdd = 1,
    kStratCmdRemove = 2,
    kStratCmdStart = 3,
    kStratCmdStop = 4,
    kStratCmdPause = 5,
    kStratCmdResume = 6
};

struct StratSpec {
    char id[24]{};
    uint32_t acct{0};
    uint32_t instrument{0};
    uint32_t instrument_b{0};
    uint8_t kind{2};
    uint16_t window{16};
    int8_t sign{1};
    int64_t threshold_ppm{10};
    int64_t qty{1};
    int64_t cooldown_us{1000000};
    int64_t ratio_ppm{1000000};
    uint8_t env{0};
};

struct StratCmd {
    uint64_t seq{0};
    uint8_t kind{0};
    uint8_t env{0};
    uint8_t pad[2]{};
    StratSpec spec{};
    char id[24]{};
};

struct StratRow {
    char id[24]{};
    uint8_t state{0};
    uint8_t kind{0};
    uint16_t window{0};
    int8_t sign{1};
    uint8_t env{0};
    uint8_t shared{0};
    uint32_t acct{0};
    uint32_t instrument{0};
    uint32_t instrument_b{0};
    int64_t threshold_ppm{0};
    int64_t qty{0};
    int64_t ratio_ppm{1000000};
    int64_t factor{0};
    int64_t position{0};
    int64_t position_b{0};
    int64_t spread{0};
    int64_t mean{0};
    int64_t sd{0};
    int64_t rounds{0};
    int64_t wins{0};
    int64_t losses{0};
    int64_t timeouts{0};
    int64_t stop_hits{0};
    uint8_t pair_ready{0};
    int64_t signals{0};
    int64_t orders{0};
    int64_t rejects{0};
    int64_t realized{0};
    int64_t unrealized{0};
    int64_t last_sig_us{0};
    int64_t started_us{0};
    char note[48]{};
};

class StrategyRegistry {
public:
    static constexpr int kMaxStrategies = 8;
    static constexpr uint32_t kCmdCap = 32;

    int used(int slot) const noexcept {
        return slot >= 0 && slot < kMaxStrategies && used_[slot] ? 1 : 0;
    }

    uint8_t state(int slot) const noexcept {
        if (slot < 0 || slot >= kMaxStrategies || !used_[slot]) return kStratIdle;
        return states_[slot];
    }

    const StratSpec& spec(int slot) const noexcept { return specs_[slot]; }

    const char* note(int slot) const noexcept {
        if (slot < 0 || slot >= kMaxStrategies) return "";
        return notes_[slot];
    }

    int64_t since(int slot) const noexcept { return since_[slot]; }

    int find(const char* id) const noexcept {
        if (!id || !id[0]) return -1;
        for (int i = 0; i < kMaxStrategies; ++i) {
            if (used_[i] && std::strcmp(specs_[i].id, id) == 0) return i;
        }
        return -1;
    }

    int active_count() const noexcept {
        int n = 0;
        for (int i = 0; i < kMaxStrategies; ++i) n += used_[i] ? 1 : 0;
        return n;
    }

    int running_count() const noexcept {
        int n = 0;
        for (int i = 0; i < kMaxStrategies; ++i) {
            if (used_[i] && states_[i] == kStratRunning) ++n;
        }
        return n;
    }

    // Trading thread only.
    int apply_add(const StratSpec& s, int64_t now_us) noexcept {
        if (!s.id[0]) return -1;
        int len = 0;
        while (len < kIdCap && s.id[len]) ++len;
        if (len >= kIdCap) return -1;
        if (find(s.id) >= 0) return -1;
        if (s.kind < 1 || s.kind > kStratPairKind) return -1;
        if (s.window < 2 || s.window > 256) return -1;
        if (s.sign != 1 && s.sign != -1) return -1;
        if (s.threshold_ppm <= 0 || s.qty <= 0 || s.cooldown_us < 0) return -1;
        if (s.kind == kStratPairKind) {
            if (s.instrument_b == 0 || s.instrument_b == s.instrument || s.ratio_ppm <= 0) return -1;
        }
        int slot = -1;
        for (int i = 0; i < kMaxStrategies; ++i) {
            if (!used_[i]) {
                slot = i;
                break;
            }
        }
        if (slot < 0) return -1;
        specs_[slot] = s;
        states_[slot] = kStratIdle;
        notes_[slot][0] = 0;
        since_[slot] = now_us;
        used_[slot] = true;
        StratRow row{};
        copy_id(row.id, s.id);
        row.state = kStratIdle;
        row.kind = s.kind;
        row.window = s.window;
        row.sign = s.sign;
        row.env = s.env;
        row.acct = s.acct;
        row.instrument = s.instrument;
        row.instrument_b = s.instrument_b;
        row.threshold_ppm = s.threshold_ppm;
        row.qty = s.qty;
        row.ratio_ppm = s.ratio_ppm;
        publish(slot, row);
        return slot;
    }

    // Applies one control command. Returns the slot, or -1 when rejected.
    int apply(const StratCmd& c, int64_t now_us) noexcept {
        if (c.kind == kStratCmdAdd) {
            return apply_add(c.spec, now_us);
        }
        const int slot = find(c.id);
        if (slot < 0) return -1;
        if (c.env != specs_[slot].env) {
            set_note(slot, "mode mismatch");
            return -1;
        }
        switch (c.kind) {
            case kStratCmdRemove: {
                StratRow empty{};
                publish(slot, empty);
                used_[slot] = false;
                states_[slot] = kStratIdle;
                specs_[slot] = StratSpec{};
                notes_[slot][0] = 0;
                return slot;
            }
            case kStratCmdStart:
                if (states_[slot] == kStratRunning || states_[slot] == kStratPaused) {
                    set_note(slot, "already active");
                    return -1;
                }
                states_[slot] = kStratRunning;
                set_note(slot, "started");
                since_[slot] = now_us;
                return slot;
            case kStratCmdPause:
                if (states_[slot] != kStratRunning) {
                    set_note(slot, "not running");
                    return -1;
                }
                states_[slot] = kStratPaused;
                set_note(slot, "paused");
                return slot;
            case kStratCmdResume:
                if (states_[slot] != kStratPaused) {
                    set_note(slot, "not paused");
                    return -1;
                }
                states_[slot] = kStratRunning;
                set_note(slot, "resumed");
                return slot;
            case kStratCmdStop:
                states_[slot] = kStratStopped;
                set_note(slot, "stopped");
                return slot;
            default:
                return -1;
        }
    }

    void publish(int slot, const StratRow& row) noexcept {
        if (slot < 0 || slot >= kMaxStrategies) return;
        Cell& c = cells_[slot];
        const uint64_t s = c.seq.load(std::memory_order_relaxed);
        c.seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        c.row = row;
        c.seq.store(s + 2, std::memory_order_release);
    }

    bool read(int slot, StratRow& out) const noexcept {
        if (slot < 0 || slot >= kMaxStrategies) return false;
        const Cell& c = cells_[slot];
        for (int spin = 0; spin < 64; ++spin) {
            const uint64_t s1 = c.seq.load(std::memory_order_acquire);
            if (s1 & 1ull) continue;
            const StratRow tmp = c.row;
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t s2 = c.seq.load(std::memory_order_acquire);
            if (s1 == s2) {
                out = tmp;
                return true;
            }
        }
        out = StratRow{};
        return false;
    }

    void set_note(int slot, const char* text) noexcept {
        if (slot < 0 || slot >= kMaxStrategies) return;
        std::snprintf(notes_[slot], sizeof notes_[slot], "%s", text ? text : "");
    }

    // HTTP thread only.
    bool submit(const StratCmd& c) noexcept { return q_.try_push(c); }
    bool take(StratCmd& c) noexcept { return q_.try_pop(c); }

    // Optional startup-config snapshot for shutdown or offline tooling.
    bool save(const char* path) const noexcept {
        if (!path || !path[0]) return false;
        std::FILE* f = std::fopen(path, "w");
        if (!f) return false;
        std::fprintf(f, "# strategy = id acct instrument kind window sign threshold_ppm qty cooldown_ms "
                        "[test|live] [b=<instrument> ratio=<ppm>]\n");
        for (int i = 0; i < kMaxStrategies; ++i) {
            if (!used_[i]) continue;
            const StratSpec& s = specs_[i];
            std::fprintf(f, "strategy = %s %u %u %u %u %d %lld %lld %lld %s", s.id, s.acct, s.instrument,
                         static_cast<unsigned>(s.kind), static_cast<unsigned>(s.window),
                         static_cast<int>(s.sign), static_cast<long long>(s.threshold_ppm),
                         static_cast<long long>(s.qty), static_cast<long long>(s.cooldown_us / 1000),
                         s.env == 0 ? "test" : "live");
            if (s.kind == kStratPairKind) {
                std::fprintf(f, " b=%u ratio=%lld", s.instrument_b, static_cast<long long>(s.ratio_ppm));
            }
            std::fputc('\n', f);
        }
        std::fclose(f);
        return true;
    }

private:
    static constexpr int kIdCap = 24;

    struct alignas(kCacheLine) Cell {
        std::atomic<uint64_t> seq{0};
        StratRow row{};
    };

    static void copy_id(char* dst, const char* src) noexcept {
        std::snprintf(dst, kIdCap, "%s", src ? src : "");
    }

    StratSpec specs_[kMaxStrategies]{};
    uint8_t states_[kMaxStrategies]{};
    char notes_[kMaxStrategies][48]{};
    int64_t since_[kMaxStrategies]{};
    bool used_[kMaxStrategies]{};
    SendJoinRing<StratCmd, kCmdCap> q_{};
    Cell cells_[kMaxStrategies]{};
};

}  // namespace pitlane
