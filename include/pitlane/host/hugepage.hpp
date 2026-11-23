#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pitlane {

// Kernel huge-page pool and THP control through sysfs/proc. Writing needs privileges.
// Library mappings request explicit huge pages (MAP_HUGETLB / hugetlbfs); these helpers
// reserve nr_hugepages and turn transparent hugepages off. THP is not used by the library.

inline bool hugepage_count_get(long& out) noexcept {
    std::FILE* f = std::fopen("/proc/sys/vm/nr_hugepages", "r");
    if (!f) return false;
    long v = 0;
    const int n = std::fscanf(f, "%ld", &v);
    std::fclose(f);
    if (n != 1) return false;
    out = v;
    return true;
}

inline bool hugepage_free_get(long& out) noexcept {
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return false;
    char line[256];
    bool ok = false;
    while (std::fgets(line, static_cast<int>(sizeof line), f)) {
        if (std::strncmp(line, "HugePages_Free:", 15) != 0) continue;
        long val = 0;
        if (std::sscanf(line + 15, "%ld", &val) == 1) {
            out = val;
            ok = true;
        }
        break;
    }
    std::fclose(f);
    return ok;
}

inline bool hugepage_count_set(long n) noexcept {
    if (unlikely(n < 0)) return false;
    std::FILE* f = std::fopen("/proc/sys/vm/nr_hugepages", "w");
    if (!f) return false;
    const int wrote = std::fprintf(f, "%ld\n", n);
    std::fclose(f);
    return wrote > 0;
}

// Ensures at least n pages are reserved. Grows the pool when short; never shrinks.
inline bool hugepage_count_ensure(long n) noexcept {
    if (unlikely(n < 0)) return false;
    long cur = 0;
    if (!hugepage_count_get(cur)) return false;
    if (cur >= n) return true;
    return hugepage_count_set(n);
}

inline bool hugepage_size_kB(long& out) noexcept {
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return false;
    char line[256];
    bool ok = false;
    while (std::fgets(line, static_cast<int>(sizeof line), f)) {
        if (std::strncmp(line, "Hugepagesize:", 13) != 0) continue;
        long val = 0;
        if (std::sscanf(line + 13, "%ld", &val) == 1) {
            out = val;
            ok = true;
        }
        break;
    }
    std::fclose(f);
    return ok;
}

namespace detail {

inline bool thp_write_never(const char* path) noexcept {
    std::FILE* f = std::fopen(path, "w");
    if (!f) return false;
    const int wrote = std::fprintf(f, "never\n");
    std::fclose(f);
    return wrote > 0;
}

inline bool thp_read_never(const char* path) noexcept {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char buf[128]{};
    const char* got = std::fgets(buf, static_cast<int>(sizeof buf), f);
    std::fclose(f);
    if (!got) return false;
    return std::strstr(buf, "[never]") != nullptr;
}

}  // namespace detail

// Writes "never" to transparent_hugepage enabled and defrag. Library mappings never use THP.
inline bool thp_disable() noexcept {
    const bool enabled = detail::thp_write_never("/sys/kernel/mm/transparent_hugepage/enabled");
    (void)detail::thp_write_never("/sys/kernel/mm/transparent_hugepage/defrag");
    return enabled;
}

inline bool thp_is_never() noexcept {
    return detail::thp_read_never("/sys/kernel/mm/transparent_hugepage/enabled");
}

// Reads the kernel command line for isolcpus= / nohz_full= tokens into out (NUL-terminated).
inline int cmdline_get(char* out, int cap) noexcept {
    if (unlikely(!out || cap < 2)) return -1;
    std::FILE* f = std::fopen("/proc/cmdline", "r");
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

inline bool cmdline_has(const char* token) noexcept {
    if (unlikely(!token || !token[0])) return false;
    char buf[1024]{};
    if (cmdline_get(buf, static_cast<int>(sizeof buf)) < 0) return false;
    return std::strstr(buf, token) != nullptr;
}

}  // namespace pitlane
