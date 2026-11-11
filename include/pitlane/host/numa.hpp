#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace pitlane {

// NUMA helpers via raw mempolicy syscalls (no libnuma). Fail closed when the
// kernel or privilege path rejects the call.

inline int numa_node_count() noexcept {
    std::FILE* f = std::fopen("/sys/devices/system/node/online", "r");
    if (!f) return 1;
    char buf[64]{};
    const char* got = std::fgets(buf, static_cast<int>(sizeof buf), f);
    std::fclose(f);
    if (!got) return 1;
    int max_node = 0;
    const char* p = buf;
    while (*p) {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p || *p == '\n') break;
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
        if (b > max_node) max_node = static_cast<int>(b);
    }
    return max_node + 1;
}

inline int numa_node_of_cpu(int cpu) noexcept {
    if (unlikely(cpu < 0)) return -1;
    char path[128];
    if (std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d", cpu) >= static_cast<int>(sizeof path)) {
        return -1;
    }
    // Resolve cpuN/node* symlink directory entries.
    char link[256];
    if (std::snprintf(link, sizeof link, "%s/node%%d", path) >= static_cast<int>(sizeof link)) return -1;
    for (int n = 0; n < 64; ++n) {
        char node_path[192];
        if (std::snprintf(node_path, sizeof node_path, "%s/node%d", path, n) >= static_cast<int>(sizeof node_path)) {
            continue;
        }
        if (::access(node_path, F_OK) == 0) return n;
    }
    return -1;
}

inline bool numa_bind_range(void* addr, std::size_t bytes, int node) noexcept {
    if (unlikely(!addr || bytes == 0 || node < 0)) return false;
    unsigned long mask = 1ul << static_cast<unsigned>(node);
    // maxnode is bit count; kernel wants one past the highest bit index.
    const unsigned long maxnode = static_cast<unsigned long>(node) + 2ul;
    const long rc = ::syscall(__NR_mbind, addr, bytes, MPOL_BIND, &mask, maxnode, MPOL_MF_MOVE | MPOL_MF_STRICT);
    return rc == 0;
}

inline bool numa_interleave_range(void* addr, std::size_t bytes) noexcept {
    if (unlikely(!addr || bytes == 0)) return false;
    const int nodes = numa_node_count();
    if (nodes <= 0) return false;
    unsigned long mask = 0;
    for (int i = 0; i < nodes && i < 64; ++i) mask |= 1ul << static_cast<unsigned>(i);
    const unsigned long maxnode = static_cast<unsigned long>(nodes) + 1ul;
    const long rc = ::syscall(__NR_mbind, addr, bytes, MPOL_INTERLEAVE, &mask, maxnode, MPOL_MF_MOVE);
    return rc == 0;
}

}  // namespace pitlane
