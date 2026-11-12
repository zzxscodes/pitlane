#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace pitlane {

// CPU affinity helpers for the calling thread or process.
// They change only the caller's placement. They do not edit grub or isolcpus.
// Build with _GNU_SOURCE (the library CMake sets it) so pthread_setaffinity_np is visible.

PITLANE_INLINE int cpu_count() noexcept {
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 0;
}

PITLANE_INLINE bool pin_thread(int cpu) noexcept {
    const int n = cpu_count();
    if (unlikely(cpu < 0 || cpu >= n)) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<size_t>(cpu), &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof set, &set) == 0;
}

PITLANE_INLINE bool pin_process(int cpu) noexcept {
    const int n = cpu_count();
    if (unlikely(cpu < 0 || cpu >= n)) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<size_t>(cpu), &set);
    return ::sched_setaffinity(0, sizeof set, &set) == 0;
}

PITLANE_INLINE bool pin_thread_mask(const int* cpus, int n) noexcept {
    if (unlikely(!cpus || n <= 0)) return false;
    const int total = cpu_count();
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < n; ++i) {
        if (cpus[i] < 0 || cpus[i] >= total) return false;
        CPU_SET(static_cast<size_t>(cpus[i]), &set);
    }
    return ::pthread_setaffinity_np(::pthread_self(), sizeof set, &set) == 0;
}

PITLANE_INLINE bool pin_process_mask(const int* cpus, int n) noexcept {
    if (unlikely(!cpus || n <= 0)) return false;
    const int total = cpu_count();
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < n; ++i) {
        if (cpus[i] < 0 || cpus[i] >= total) return false;
        CPU_SET(static_cast<size_t>(cpus[i]), &set);
    }
    return ::sched_setaffinity(0, sizeof set, &set) == 0;
}

// Reads /sys/devices/system/cpu/isolated into out as CPU ids. Returns the count, or -1.
inline int isolated_cpus(int* out, int cap) noexcept {
    if (unlikely(!out || cap <= 0)) return -1;
    std::FILE* f = std::fopen("/sys/devices/system/cpu/isolated", "r");
    if (!f) return -1;
    char buf[256]{};
    const char* got = std::fgets(buf, static_cast<int>(sizeof buf), f);
    std::fclose(f);
    if (!got) return 0;
    int n = 0;
    const char* p = buf;
    while (*p && n < cap) {
        while (*p == ' ' || *p == ',' || *p == '\n') ++p;
        if (!*p) break;
        char* end = nullptr;
        const long a = std::strtol(p, &end, 10);
        if (end == p) break;
        p = end;
        long b = a;
        if (*p == '-') {
            ++p;
            b = std::strtol(p, &end, 10);
            if (end == p) break;
            p = end;
        }
        for (long c = a; c <= b && n < cap; ++c) out[n++] = static_cast<int>(c);
    }
    return n;
}

PITLANE_INLINE int current_cpu() noexcept {
    const int c = ::sched_getcpu();
    return c >= 0 ? c : -1;
}

}  // namespace pitlane
