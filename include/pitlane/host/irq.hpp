#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pitlane {

// IRQ affinity helpers through /proc. They need privileges to write.
// They do not stop irqbalance or edit grub. A refused write returns false.

inline bool irq_affinity_list_set(int irq, const int* cpus, int n) noexcept {
    if (unlikely(irq < 0 || !cpus || n <= 0)) return false;
    char path[64];
    if (std::snprintf(path, sizeof path, "/proc/irq/%d/smp_affinity_list", irq) <= 0) return false;
    char body[128]{};
    int at = 0;
    for (int i = 0; i < n; ++i) {
        if (cpus[i] < 0) return false;
        const int wrote =
            std::snprintf(body + at, sizeof body - static_cast<std::size_t>(at), i ? ",%d" : "%d", cpus[i]);
        if (wrote <= 0) return false;
        at += wrote;
        if (at >= static_cast<int>(sizeof body) - 1) return false;
    }
    std::FILE* f = std::fopen(path, "w");
    if (!f) return false;
    const bool ok = std::fputs(body, f) >= 0;
    std::fclose(f);
    return ok;
}

inline int irq_affinity_list_get(int irq, char* out, int cap) noexcept {
    if (unlikely(irq < 0 || !out || cap < 2)) return -1;
    char path[64];
    if (std::snprintf(path, sizeof path, "/proc/irq/%d/smp_affinity_list", irq) <= 0) return -1;
    std::FILE* f = std::fopen(path, "r");
    if (!f) return -1;
    if (!std::fgets(out, cap, f)) {
        std::fclose(f);
        return -1;
    }
    std::fclose(f);
    int len = static_cast<int>(std::strlen(out));
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
        out[len - 1] = 0;
        --len;
    }
    return len;
}

// Writes IRQ numbers from /proc/interrupts into out_irqs. Returns the count, or -1.
inline int irq_list(int* out_irqs, int cap) noexcept {
    if (unlikely(!out_irqs || cap <= 0)) return -1;
    std::FILE* f = std::fopen("/proc/interrupts", "r");
    if (!f) return -1;
    char line[512];
    if (!std::fgets(line, sizeof line, f)) {
        std::fclose(f);
        return -1;
    }
    int n = 0;
    while (n < cap && std::fgets(line, sizeof line, f)) {
        char* end = nullptr;
        const long irq = std::strtol(line, &end, 10);
        if (end == line || *end != ':') continue;
        out_irqs[n++] = static_cast<int>(irq);
    }
    std::fclose(f);
    return n;
}

}  // namespace pitlane
