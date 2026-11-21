#pragma once

// 性能基线：把一次基准运行的 p50/p99/最大值按名字对齐保存，下一次运行逐项比较，
// 输出相对变化 ppm 与是否回归。用于让每次提交的延迟变化可量化、可追溯。
//
// 序列化格式是稳定的一行一项：
//   name,n,p50,p99,p999,max
// 版本行以 # 开头，包含提交号与时间，便于把基线和提交绑定。

#include "pitlane/time/mag.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

inline constexpr int kPerfRows = 32;
inline constexpr int kPerfName = 32;

struct PerfLine {
    char name[kPerfName]{};
    uint64_t n{0};
    uint64_t p50{0};
    uint64_t p99{0};
    uint64_t p999{0};
    uint64_t max{0};
};

struct PerfDiff {
    char name[kPerfName]{};
    uint64_t base_p99{0};
    uint64_t cur_p99{0};
    int64_t delta_ppm{0}; // 正数表示变慢
    uint8_t regressed{0};
    uint8_t missing{0};
};

class PerfBaseline {
public:
    void clear() noexcept { n_ = 0; }

    int count() const noexcept { return n_; }
    const PerfLine& at(int i) const noexcept { return rows_[i]; }

    int find(const char* name) const noexcept {
        if (!name) return -1;
        for (int i = 0; i < n_; ++i) {
            if (std::strcmp(rows_[i].name, name) == 0) return i;
        }
        return -1;
    }

    bool put(const char* name, const LatReport& r) noexcept {
        if (!name || !name[0] || r.n == 0) return false;
        int i = find(name);
        if (i < 0) {
            if (n_ >= kPerfRows) return false;
            i = n_++;
            std::snprintf(rows_[i].name, kPerfName, "%s", name);
        }
        rows_[i].n = r.n;
        rows_[i].p50 = r.p50;
        rows_[i].p99 = r.p99;
        rows_[i].p999 = r.p999;
        rows_[i].max = r.max;
        return true;
    }

    bool put(const char* name, const MagHist& h) noexcept {
        LatReport r{};
        if (!mag_report(h, r)) return false;
        return put(name, r);
    }

    // 逐项比较。tolerance_ppm 以内的变慢不算回归。返回 regressed 数量。
    int compare(const PerfBaseline& cur, int64_t tolerance_ppm, PerfDiff* out, int cap) const noexcept {
        int bad = 0;
        int n = 0;
        for (int i = 0; i < n_; ++i) {
            const int j = cur.find(rows_[i].name);
            if (out && n < cap) {
                PerfDiff& d = out[n];
                d = PerfDiff{};
                std::snprintf(d.name, kPerfName, "%s", rows_[i].name);
                d.base_p99 = rows_[i].p99;
                if (j < 0) {
                    d.missing = 1;
                } else {
                    d.cur_p99 = cur.rows_[j].p99;
                    d.delta_ppm = delta_ppm_of(rows_[i].p99, cur.rows_[j].p99);
                    d.regressed = d.delta_ppm > tolerance_ppm ? 1 : 0;
                }
                ++n;
            }
            if (j < 0) continue;
            const int64_t d = delta_ppm_of(rows_[i].p99, cur.rows_[j].p99);
            if (d > tolerance_ppm) ++bad;
        }
        if (out && n < cap) {
            for (int j = 0; j < cur.n_ && n < cap; ++j) {
                if (find(cur.rows_[j].name) >= 0) continue;
                PerfDiff& d = out[n++];
                d = PerfDiff{};
                std::snprintf(d.name, kPerfName, "%s", cur.rows_[j].name);
                d.cur_p99 = cur.rows_[j].p99;
                d.regressed = 0;
            }
        }
        return bad;
    }

    int to_text(char* out, int cap, const char* rev = nullptr, int64_t epoch_s = 0) const noexcept {
        if (!out || cap <= 0) return 0;
        int used = 0;
        used += std::snprintf(out + used, static_cast<std::size_t>(cap - used), "# pitlane perf-baseline rev=%s epoch=%lld\n",
                              rev ? rev : "-", static_cast<long long>(epoch_s));
        for (int i = 0; i < n_ && used < cap; ++i) {
            used += std::snprintf(out + used, static_cast<std::size_t>(cap - used), "%s,%llu,%llu,%llu,%llu,%llu\n",
                                  rows_[i].name, static_cast<unsigned long long>(rows_[i].n),
                                  static_cast<unsigned long long>(rows_[i].p50),
                                  static_cast<unsigned long long>(rows_[i].p99),
                                  static_cast<unsigned long long>(rows_[i].p999),
                                  static_cast<unsigned long long>(rows_[i].max));
        }
        return used < cap ? used : cap;
    }

    bool from_text(const char* text, int len) noexcept {
        if (!text || len <= 0) return false;
        clear();
        int i = 0;
        while (i < len) {
            int end = i;
            while (end < len && text[end] != '\n') ++end;
            if (text[i] != '#' && end > i) {
                char line[160];
                const int m = end - i < static_cast<int>(sizeof line) - 1 ? end - i : static_cast<int>(sizeof line) - 1;
                std::memcpy(line, text + i, static_cast<std::size_t>(m));
                line[m] = 0;
                PerfLine row{};
                unsigned long long a = 0, b = 0, c = 0, d = 0, e = 0;
                char name[kPerfName]{};
                if (std::sscanf(line, "%31[^,],%llu,%llu,%llu,%llu,%llu", name, &a, &b, &c, &d, &e) == 6) {
                    if (n_ < kPerfRows) {
                        std::snprintf(row.name, kPerfName, "%s", name);
                        row.n = a;
                        row.p50 = b;
                        row.p99 = c;
                        row.p999 = d;
                        row.max = e;
                        rows_[n_++] = row;
                    }
                }
            }
            i = end + 1;
        }
        return n_ > 0;
    }

    static int64_t delta_ppm_of(uint64_t base, uint64_t cur) noexcept {
        if (base == 0) return cur == 0 ? 0 : INT64_MAX;
        const int64_t diff = static_cast<int64_t>(cur) - static_cast<int64_t>(base);
        return diff * 1000000ll / static_cast<int64_t>(base);
    }

private:
    PerfLine rows_[kPerfRows]{};
    int n_{0};
};

} // namespace pitlane
