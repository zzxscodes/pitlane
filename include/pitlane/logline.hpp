#pragma once

#include <cstdint>

namespace pitlane {

// min_level is the most verbose level still emitted. 0 error, 1 warn, 2 info, 3 debug.
class Log {
public:
    explicit Log(int min_level) noexcept : min_(min_level) {}
    void bind(void (*fn)(int level, const char* line, void* user), void* user) noexcept;
    void write(int level, const char* line) noexcept;
    uint64_t emitted() const noexcept { return emitted_; }
    uint64_t dropped() const noexcept { return dropped_; }

private:
    int min_{2};
    void (*fn_)(int, const char*, void*){nullptr};
    void* user_{nullptr};
    uint64_t emitted_{0};
    uint64_t dropped_{0};
};

}  // namespace pitlane
