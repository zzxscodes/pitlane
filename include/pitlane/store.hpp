#pragma once

#include <cstdint>

namespace pitlane {

class Dict {
public:
    static constexpr int kCap = 64;
    int intern(const char* name) noexcept;
    int find(const char* name) const noexcept;
    const char* text(int id) const noexcept;
    int size() const noexcept { return n_; }

private:
    char text_[kCap][32]{};
    int n_{0};
};

template <int N>
class Column {
public:
    bool put(int index, int64_t value) noexcept {
        if (index < 0 || index >= N) return false;
        value_[index] = value;
        used_[index] = 1;
        return true;
    }
    bool get(int index, int64_t* value) const noexcept {
        if (index < 0 || index >= N || !used_[index]) return false;
        if (value) *value = value_[index];
        return true;
    }

private:
    int64_t value_[N]{};
    uint8_t used_[N]{};
};

class TimeIndex {
public:
    static constexpr int kCap = 256;
    bool put(int64_t ts, uint32_t row) noexcept;
    bool get(int64_t ts, uint32_t* row) const noexcept;

private:
    int64_t key_[kCap]{};
    uint32_t row_[kCap]{};
    uint8_t used_[kCap]{};
};

}  // namespace pitlane
