#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

#include <unistd.h>

namespace pitlane {

// Cold-path page sizes for library mappings. Ordinary page from sysconf;
// explicit huge-page size from /proc/meminfo, else 2 MiB.

inline std::size_t page_size_bytes() noexcept {
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) return 4096u;
    return static_cast<std::size_t>(page);
}

inline std::size_t hugepage_size_bytes() noexcept {
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (std::fgets(line, static_cast<int>(sizeof line), f)) {
            if (std::strncmp(line, "Hugepagesize:", 13) != 0) continue;
            long val = 0;
            if (std::sscanf(line + 13, "%ld", &val) == 1 && val > 0) {
                std::fclose(f);
                return static_cast<std::size_t>(val) * 1024u;
            }
            break;
        }
        std::fclose(f);
    }
    return 2u * 1024u * 1024u;
}

inline std::size_t round_up_bytes(std::size_t n, std::size_t align) noexcept {
    if (align == 0) return n;
    return (n + align - 1) / align * align;
}

}  // namespace pitlane
