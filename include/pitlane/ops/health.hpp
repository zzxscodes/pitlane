#pragma once

// Host health sampling and threshold evaluation.
//
// `health_sample` reads CPU, memory, load, threads, file descriptors, disk,
// network, and uptime from /proc and statfs. Rates are deltas against the
// previous call. `health_beat` records a heartbeat whose age feeds the
// heartbeat limit, so a stalled trading loop raises its own bit.
//
// The sampler keeps one process-wide context and is not thread-safe: a
// dedicated sampler thread reads it once per second, while the trading thread
// records beats through the atomic `beat_us` and evaluates published samples.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <sys/vfs.h>

namespace pitlane {

struct HealthSample {
    int32_t cpu_ppm{0};
    int32_t mem_ppm{0};
    int32_t load_ppm{0};
    int32_t threads{0};
    int32_t fds{0};
    int32_t disk_used_ppm{0};
    int64_t rx_bps{0};
    int64_t tx_bps{0};
    int64_t uptime_s{0};
    int64_t heartbeat_us{0};  // age of the last beat
};

struct HealthLimits {
    int32_t cpu_ppm{900000};
    int32_t mem_ppm{900000};
    int32_t disk_ppm{900000};
    int32_t fds{4096};
    int64_t heartbeat_us{2000000};
};

enum HealthBit : uint8_t {
    kHealthCpu = 1,
    kHealthMem = 2,
    kHealthDisk = 4,
    kHealthFds = 8,
    kHealthHeartbeat = 16,
};

// level: 0 ok, 1 warn (at or above 80% of a limit), 2 critical (at or above
// the limit). `bits` lists the metrics that tripped on the latest evaluation.
struct HealthState {
    uint8_t level{0};
    uint8_t bits{0};
};

namespace detail {

struct HealthCtx {
    uint64_t cpu_total{0};
    uint64_t cpu_idle{0};
    uint64_t rx{0};
    uint64_t tx{0};
    int64_t stamp_us{0};
    std::atomic<int64_t> beat_us{0};
    bool primed{false};
};

inline HealthCtx& health_ctx() noexcept {
    static HealthCtx ctx;
    return ctx;
}

inline bool read_line(const char* path, char* buf, int cap) noexcept {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;
    const bool ok = std::fgets(buf, cap, f) != nullptr;
    std::fclose(f);
    return ok;
}

inline int64_t uptime_seconds() noexcept {
    char buf[128];
    if (!read_line("/proc/uptime", buf, sizeof buf)) return 0;
    return static_cast<int64_t>(std::strtod(buf, nullptr));
}

inline int count_fds() noexcept {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return 0;
    int n = 0;
    while (struct dirent* e = ::readdir(d)) {
        if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) ++n;
    }
    ::closedir(d);
    return n;
}

inline int count_threads() noexcept {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, "Threads:", 8) == 0) {
            n = static_cast<int>(std::strtol(line + 8, nullptr, 10));
            break;
        }
    }
    std::fclose(f);
    return n;
}

inline int32_t load_ppm() noexcept {
    char buf[128];
    if (!read_line("/proc/loadavg", buf, sizeof buf)) return 0;
    const double v = std::strtod(buf, nullptr);
    if (v <= 0) return 0;
    return static_cast<int32_t>(v * 1000000.0);
}

// Sums rx and tx bytes over every interface except loopback.
inline void net_totals(uint64_t& rx, uint64_t& tx) noexcept {
    rx = 0;
    tx = 0;
    std::FILE* f = std::fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[512];
    bool header = true;
    while (std::fgets(line, sizeof line, f)) {
        if (header) {
            header = false;
            continue;
        }
        char* colon = std::strchr(line, ':');
        if (!colon) continue;
        char* p = line;
        while (*p == ' ') ++p;
        if (std::strncmp(p, "lo:", 3) == 0) continue;
        p = colon + 1;
        uint64_t v[16] = {};
        int n = 0;
        while (n < 16) {
            char* end = nullptr;
            const unsigned long long x = std::strtoull(p, &end, 10);
            if (end == p) break;
            v[n++] = static_cast<uint64_t>(x);
            p = end;
        }
        if (n >= 9) {
            rx += v[0];
            tx += v[8];
        }
    }
    std::fclose(f);
}

}  // namespace detail

// Records a heartbeat. Call it from the trading loop; the age becomes
// `heartbeat_us` on the next sample.
inline void health_beat(int64_t now_us) noexcept {
    detail::health_ctx().beat_us.store(now_us, std::memory_order_relaxed);
}

// Samples the host. Returns false when /proc/stat cannot be read, which leaves
// the output untouched.
inline bool health_sample(HealthSample& out, int64_t now_us) noexcept {
    detail::HealthCtx& ctx = detail::health_ctx();

    std::FILE* f = std::fopen("/proc/stat", "r");
    if (!f) return false;
    char line[512];
    if (!std::fgets(line, sizeof line, f)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    uint64_t v[10] = {};
    int n = 0;
    char* p = line;
    while (*p && *p != ' ') ++p;  // skip "cpu"
    while (n < 10) {
        char* end = nullptr;
        const unsigned long long x = std::strtoull(p, &end, 10);
        if (end == p) break;
        v[n++] = static_cast<uint64_t>(x);
        p = end;
    }
    if (n < 4) return false;
    uint64_t total = 0;
    for (int i = 0; i < n && i < 8; ++i) total += v[i];
    const uint64_t idle = v[3] + (n > 4 ? v[4] : 0);
    if (ctx.primed && total > ctx.cpu_total) {
        const uint64_t dt = total - ctx.cpu_total;
        const uint64_t di = idle - ctx.cpu_idle;
        const int64_t busy = static_cast<int64_t>(dt - di);
        out.cpu_ppm = static_cast<int32_t>(busy * 1000000 / static_cast<int64_t>(dt));
    } else if (!ctx.primed) {
        out.cpu_ppm = 0;
    }
    ctx.cpu_total = total;
    ctx.cpu_idle = idle;

    // Memory: prefer MemAvailable, fall back to MemFree plus buffers.
    {
        std::FILE* mf = std::fopen("/proc/meminfo", "r");
        uint64_t total_kb = 0;
        uint64_t avail_kb = 0;
        uint64_t free_kb = 0;
        if (mf) {
            char mline[256];
            while (std::fgets(mline, sizeof mline, mf)) {
                if (std::strncmp(mline, "MemTotal:", 9) == 0) {
                    total_kb = std::strtoull(mline + 9, nullptr, 10);
                } else if (std::strncmp(mline, "MemAvailable:", 13) == 0) {
                    avail_kb = std::strtoull(mline + 13, nullptr, 10);
                } else if (std::strncmp(mline, "MemFree:", 8) == 0) {
                    free_kb = std::strtoull(mline + 8, nullptr, 10);
                }
            }
            std::fclose(mf);
        }
        if (total_kb > 0) {
            const uint64_t avail = avail_kb > 0 ? avail_kb : free_kb;
            const uint64_t used = avail < total_kb ? total_kb - avail : 0;
            out.mem_ppm = static_cast<int32_t>(used * 1000000 / total_kb);
        }
    }

    out.load_ppm = detail::load_ppm();
    out.threads = detail::count_threads();
    out.fds = detail::count_fds();
    out.uptime_s = detail::uptime_seconds();

    struct statfs st {};
    if (::statfs("/", &st) == 0) {
        const uint64_t total_b = static_cast<uint64_t>(st.f_blocks) * static_cast<uint64_t>(st.f_bsize);
        const uint64_t used_b =
            (static_cast<uint64_t>(st.f_blocks) - static_cast<uint64_t>(st.f_bfree)) * static_cast<uint64_t>(st.f_bsize);
        if (total_b > 0) out.disk_used_ppm = static_cast<int32_t>(used_b * 1000000 / total_b);
    }

    uint64_t rx = 0;
    uint64_t tx = 0;
    detail::net_totals(rx, tx);
    if (ctx.primed && now_us > ctx.stamp_us) {
        const int64_t dt = now_us - ctx.stamp_us;
        out.rx_bps = static_cast<int64_t>((rx - ctx.rx) * 1000000ull / static_cast<uint64_t>(dt));
        out.tx_bps = static_cast<int64_t>((tx - ctx.tx) * 1000000ull / static_cast<uint64_t>(dt));
    }
    ctx.rx = rx;
    ctx.tx = tx;
    ctx.stamp_us = now_us;
    ctx.primed = true;

    const int64_t beat = ctx.beat_us.load(std::memory_order_relaxed);
    out.heartbeat_us = beat > 0 && now_us >= beat ? now_us - beat : 0;
    return true;
}

inline void health_eval(const HealthSample& s, const HealthLimits& lim, HealthState& st) noexcept {
    st.bits = 0;
    st.level = 0;
    auto check = [&st](int64_t v, int64_t limit, uint8_t bit) {
        if (limit <= 0) return;
        if (v >= limit) {
            st.bits |= bit;
            st.level = 2;
        } else if (v >= limit * 8 / 10) {
            st.bits |= bit;
            if (st.level < 1) st.level = 1;
        }
    };
    check(s.cpu_ppm, lim.cpu_ppm, kHealthCpu);
    check(s.mem_ppm, lim.mem_ppm, kHealthMem);
    check(s.disk_used_ppm, lim.disk_ppm, kHealthDisk);
    check(s.fds, lim.fds, kHealthFds);
    check(s.heartbeat_us, lim.heartbeat_us, kHealthHeartbeat);
}

}  // namespace pitlane
