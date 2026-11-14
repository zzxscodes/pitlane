#pragma once

// Research product consumed by backtest and live. Cold path only: the hot
// loop reads the resolved FlowCfg once at open().

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pitlane {

struct StudyCfg {
    uint8_t kind{2}; // FactorKind::Momentum
    uint16_t window{16};
    int8_t sign{1}; // 1 trades the factor, -1 trades its inverse (negative IC)
    int64_t signal_ppm{10};
    int horizon_bars{8};
    int64_t samples{0};
    int64_t ic_ppm{0};
    int64_t rank_ic_ppm{0};
    int64_t hit_rate_ppm{0};
};

inline char* study_trim(char* s) noexcept {
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) return s;
    char* e = s + std::strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    *e = 0;
    return s;
}

inline bool study_i64(const char* s, int64_t& out) noexcept {
    if (!s || !*s) return false;
    char* end = nullptr;
    const long long v = std::strtoll(s, &end, 10);
    if (!end || *end) return false;
    out = static_cast<int64_t>(v);
    return true;
}

inline bool study_conf_save(const char* path, const StudyCfg& cfg) {
    if (!path || !path[0]) return false;
    std::FILE* f = std::fopen(path, "w");
    if (!f) {
        std::fprintf(stderr, "study conf: open %s failed\n", path);
        return false;
    }
    std::fprintf(f, "# pitlane research product v1\n");
    std::fprintf(f, "factor_kind = %u\n", static_cast<unsigned>(cfg.kind));
    std::fprintf(f, "factor_window = %u\n", static_cast<unsigned>(cfg.window));
    std::fprintf(f, "factor_sign = %d\n", static_cast<int>(cfg.sign));
    std::fprintf(f, "signal_ppm = %lld\n", static_cast<long long>(cfg.signal_ppm));
    std::fprintf(f, "horizon_bars = %d\n", cfg.horizon_bars);
    std::fprintf(f, "samples = %lld\n", static_cast<long long>(cfg.samples));
    std::fprintf(f, "ic_ppm = %lld\n", static_cast<long long>(cfg.ic_ppm));
    std::fprintf(f, "rank_ic_ppm = %lld\n", static_cast<long long>(cfg.rank_ic_ppm));
    std::fprintf(f, "hit_rate_ppm = %lld\n", static_cast<long long>(cfg.hit_rate_ppm));
    const bool ok = std::fflush(f) == 0;
    if (std::fclose(f) != 0 || !ok) {
        std::fprintf(stderr, "study conf: write %s failed\n", path);
        return false;
    }
    return true;
}

// Loads a research product. Unknown keys are ignored so the same file can
// also carry account or venue settings. Any bad value rejects the whole file.
inline bool study_conf_load(const char* path, StudyCfg& cfg) {
    if (!path || !path[0]) return false;
    std::FILE* f = std::fopen(path, "r");
    if (!f) return false;
    StudyCfg out{};
    char line[512];
    bool bad = false;
    while (!bad && std::fgets(line, sizeof line, f)) {
        char* p = study_trim(line);
        if (!*p || *p == '#' || *p == ';' || *p == '[') continue;
        char* eq = std::strchr(p, '=');
        if (!eq) {
            bad = true;
            break;
        }
        *eq = 0;
        char* key = study_trim(p);
        char* val = study_trim(eq + 1);
        int64_t v = 0;
        if (std::strcmp(key, "factor_kind") == 0) {
            if (!study_i64(val, v)) bad = true;
            else out.kind = static_cast<uint8_t>(v);
        } else if (std::strcmp(key, "factor_window") == 0) {
            if (!study_i64(val, v)) bad = true;
            else out.window = static_cast<uint16_t>(v);
        } else if (std::strcmp(key, "factor_sign") == 0) {
            if (!study_i64(val, v)) bad = true;
            else out.sign = static_cast<int8_t>(v);
        } else if (std::strcmp(key, "signal_ppm") == 0) {
            if (!study_i64(val, v)) bad = true;
            else out.signal_ppm = v;
        } else if (std::strcmp(key, "horizon_bars") == 0) {
            if (!study_i64(val, v)) bad = true;
            else out.horizon_bars = static_cast<int>(v);
        } else if (std::strcmp(key, "samples") == 0) {
            if (!study_i64(val, v)) bad = true;
            else out.samples = v;
        } else if (std::strcmp(key, "ic_ppm") == 0) {
            if (!study_i64(val, v)) bad = true;
            else out.ic_ppm = v;
        } else if (std::strcmp(key, "rank_ic_ppm") == 0) {
            if (!study_i64(val, v)) bad = true;
            else out.rank_ic_ppm = v;
        } else if (std::strcmp(key, "hit_rate_ppm") == 0) {
            if (!study_i64(val, v)) bad = true;
            else out.hit_rate_ppm = v;
        }
    }
    std::fclose(f);
    if (bad) return false;
    if (out.kind < 1 || out.kind > 8) return false;
    if (out.window < 2 || out.window > 2048) return false;
    if (out.sign != 1 && out.sign != -1) return false;
    if (out.signal_ppm < 1 || out.signal_ppm > 1000000000ll) return false;
    if (out.horizon_bars < 1 || out.horizon_bars > 1024) return false;
    cfg = out;
    return true;
}

} // namespace pitlane
