#pragma once

#include <cstdint>

namespace pitlane {

struct HostSample {
    int32_t cpu_ppm{0};
    int32_t mem_ppm{0};
    int32_t load_1m_ppm{0};
    int32_t threads{0};
};

// Samples /proc/stat, /proc/meminfo, /proc/loadavg, and /proc/self/status.
// The first call records the CPU baseline and reports zero percent.
bool host_sample(HostSample& out) noexcept;

}  // namespace pitlane
