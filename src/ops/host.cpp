#include "pitlane/ops/host.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pitlane {

namespace {

struct CpuBaseline {
    unsigned long long busy{0};
    unsigned long long total{0};
    bool valid{false};
};

CpuBaseline g_cpu{};

bool read_cpu(unsigned long long& busy, unsigned long long& total) noexcept {
    std::FILE* f = std::fopen("/proc/stat", "r");
    if (!f) return false;
    char line[256];
    bool ok = false;
    if (std::fgets(line, sizeof line, f)) {
        unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, soft = 0, steal = 0;
        const int n = std::sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &sys, &idle,
                                  &iowait, &irq, &soft, &steal);
        if (n >= 4) {
            const unsigned long long idle_all = idle + iowait;
            busy = user + nice + sys + irq + soft + steal;
            total = busy + idle_all;
            ok = true;
        }
    }
    std::fclose(f);
    return ok;
}

bool read_mem(int32_t& ppm) noexcept {
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return false;
    char line[256];
    unsigned long long total = 0;
    unsigned long long avail = 0;
    while (std::fgets(line, sizeof line, f)) {
        unsigned long long kb = 0;
        if (std::sscanf(line, "MemTotal: %llu kB", &kb) == 1) total = kb;
        else if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) avail = kb;
    }
    std::fclose(f);
    if (total == 0) return false;
    if (avail > total) avail = total;
    ppm = static_cast<int32_t>((total - avail) * 1000000ull / total);
    return true;
}

int32_t read_load() noexcept {
    std::FILE* f = std::fopen("/proc/loadavg", "r");
    if (!f) return 0;
    char line[128];
    int32_t ppm = 0;
    if (std::fgets(line, sizeof line, f)) {
        const double v = std::strtod(line, nullptr);
        if (v > 0) {
            const double scaled = v * 1000000.0;
            ppm = scaled > 2147483647.0 ? 2147483647 : static_cast<int32_t>(scaled);
        }
    }
    std::fclose(f);
    return ppm;
}

int32_t read_threads() noexcept {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    int32_t n = 0;
    while (std::fgets(line, sizeof line, f)) {
        if (std::sscanf(line, "Threads: %d", &n) == 1) break;
    }
    std::fclose(f);
    return n;
}

}  // namespace

bool host_sample(HostSample& out) noexcept {
    out = HostSample{};
    unsigned long long busy = 0;
    unsigned long long total = 0;
    if (read_cpu(busy, total)) {
        if (g_cpu.valid && total > g_cpu.total) {
            const unsigned long long db = busy - g_cpu.busy;
            const unsigned long long dt = total - g_cpu.total;
            const unsigned long long ppm = dt > 0 ? db * 1000000ull / dt : 0;
            out.cpu_ppm = static_cast<int32_t>(ppm > 1000000ull ? 1000000ull : ppm);
        }
        g_cpu.busy = busy;
        g_cpu.total = total;
        g_cpu.valid = true;
    }
    (void)read_mem(out.mem_ppm);
    out.load_1m_ppm = read_load();
    out.threads = read_threads();
    return true;
}

}  // namespace pitlane
