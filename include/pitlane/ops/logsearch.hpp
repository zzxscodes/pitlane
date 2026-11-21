#pragma once

// 日志检索：进程内固定环，记录时间、级别、类别和一行文本，支持按时间、级别、
// 类别和大小写不敏感子串过滤，结果按新到旧输出。热路径只做一次拷贝，不分配。
// 需要长期留存时由调用方把环转储到文件，这里不碰磁盘。

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

enum LogLevel : uint8_t {
    kLogDebug = 0,
    kLogInfo = 1,
    kLogWarn = 2,
    kLogError = 3
};

inline const char* log_level_name(uint8_t l) noexcept {
    switch (l) {
        case kLogWarn: return "warn";
        case kLogError: return "error";
        case kLogInfo: return "info";
        default: return "debug";
    }
}

enum LogKind : uint8_t {
    kLogOther = 0,
    kLogOrder = 1,
    kLogRisk = 2,
    kLogMarket = 3,
    kLogOps = 4,
    kLogPerf = 5
};

inline const char* log_kind_name(uint8_t k) noexcept {
    switch (k) {
        case kLogOrder: return "order";
        case kLogRisk: return "risk";
        case kLogMarket: return "market";
        case kLogOps: return "ops";
        case kLogPerf: return "perf";
        default: return "other";
    }
}

inline int8_t log_kind_id(const char* s) noexcept {
    if (!s || !s[0]) return -1;
    if (std::strcmp(s, "other") == 0) return kLogOther;
    if (std::strcmp(s, "order") == 0) return kLogOrder;
    if (std::strcmp(s, "risk") == 0) return kLogRisk;
    if (std::strcmp(s, "market") == 0) return kLogMarket;
    if (std::strcmp(s, "ops") == 0) return kLogOps;
    if (std::strcmp(s, "perf") == 0) return kLogPerf;
    return -1;
}

inline int8_t log_level_id(const char* s) noexcept {
    if (!s || !s[0]) return -1;
    if (std::strcmp(s, "debug") == 0) return kLogDebug;
    if (std::strcmp(s, "info") == 0) return kLogInfo;
    if (std::strcmp(s, "warn") == 0) return kLogWarn;
    if (std::strcmp(s, "error") == 0) return kLogError;
    return -1;
}

struct LogLine {
    int64_t ts_us{0};
    uint64_t seq{0};
    uint8_t level{kLogInfo};
    uint8_t kind{kLogOther};
    uint16_t len{0};
    char text[168]{};
};

class LogIndex {
public:
    static constexpr int kLines = 512;
    static constexpr int kText = 168;

    void clear() noexcept {
        head_ = 0;
        n_ = 0;
        seq_ = 0;
    }

    void add(int64_t ts_us, uint8_t level, uint8_t kind, const char* text) noexcept {
        LogLine& l = ring_[head_];
        l = LogLine{};
        l.ts_us = ts_us;
        l.seq = ++seq_;
        l.level = level;
        l.kind = kind;
        if (text) {
            std::snprintf(l.text, kText, "%s", text);
            l.len = static_cast<uint16_t>(std::strlen(l.text));
        }
        head_ = (head_ + 1) % kLines;
        if (n_ < kLines) ++n_;
    }

    // needle 为空表示不按文本过滤；kind 为 0xff 表示不限类别。
    int search(const char* needle, int64_t since_us, uint8_t min_level, uint8_t kind, LogLine* out, int cap) const noexcept {
        if (!out || cap <= 0) return 0;
        int n = 0;
        for (int i = 1; i <= n_ && n < cap; ++i) {
            const int at = (head_ - i + kLines * 2) % kLines;
            const LogLine& l = ring_[at];
            if (l.ts_us < since_us) continue;
            if (l.level < min_level) continue;
            if (kind != 0xff && l.kind != kind) continue;
            if (needle && needle[0] && !ci_contains(l.text, needle)) continue;
            out[n++] = l;
        }
        return n;
    }

    int count() const noexcept { return n_; }
    uint64_t seq() const noexcept { return seq_; }

    // 最近一行的序号，供增量拉取判断。
    uint64_t newest_seq() const noexcept { return seq_; }

    // 追加到缓冲区（新到旧），每行 "seq,ts,level,kind,text\n"。
    int dump(char* out, int cap) const noexcept {
        if (!out || cap <= 0) return 0;
        int used = 0;
        for (int i = 0; i < n_ && used < cap; ++i) {
            const int at = (head_ - 1 - i + kLines * 2) % kLines;
            const LogLine& l = ring_[at];
            used += std::snprintf(out + used, static_cast<std::size_t>(cap - used), "%llu,%lld,%s,%s,%s\n",
                                  static_cast<unsigned long long>(l.seq), static_cast<long long>(l.ts_us),
                                  log_level_name(l.level), log_kind_name(l.kind), l.text);
        }
        return used < cap ? used : cap;
    }

private:
    static bool ci_contains(const char* hay, const char* needle) noexcept {
        const std::size_t n = std::strlen(needle);
        if (n == 0) return true;
        for (const char* p = hay; *p; ++p) {
            std::size_t i = 0;
            while (i < n && p[i] && lower(p[i]) == lower(needle[i])) ++i;
            if (i == n) return true;
        }
        return false;
    }

    static char lower(char ch) noexcept { return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch; }

    LogLine ring_[kLines]{};
    int head_{0};
    int n_{0};
    uint64_t seq_{0};
};

} // namespace pitlane
