#pragma once

// 适配器选择与接入点。交易系统只认名字，每个适配器的实现在 adapters/<name>/，
// 它自己的连接参数/账号也放在 adapters/<name>/ 自己的 conf 文件里。
//
// 新增一个适配器：
//   1. 在 adapters/<name>/ 下实现行情、交易回调，并放一个 config.hpp 解析
//      该柜台自己的键（参考 adapters/ctp/config.hpp）；
//   2. 在 adapter_supported 放行，并在 kAdapterNames 登记；
//   3. 系统 conf 写 adapter = <name>；适配器配置默认取系统 conf 同目录的
//      adapters/<name>.conf，可用 adapter_conf = FILE 覆盖。
// 宿主只读 adapter / adapter_conf，不解释柜台私有键。未编入二进制的适配器
// 会在启动时被拒绝并列出可选值，不会静默回退。

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace pitlane {
namespace trading {

inline constexpr const char* kDefaultAdapter = "ctp";
inline constexpr const char* kAdapterNames = "ctp";

inline bool adapter_supported(const char* name) noexcept {
    return name != nullptr && std::strcmp(name, kDefaultAdapter) == 0;
}

// 适配器配置的默认位置：系统 conf 同目录下的 adapters/<name>.conf。
// 系统 conf 没有目录前缀时，按当前工作目录下的 adapters/<name>.conf 解析。
inline void adapter_conf_default(const char* system_conf, const char* name, char* out, std::size_t cap) noexcept {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!name || !*name) return;
    char dir[384];
    dir[0] = '\0';
    if (system_conf && *system_conf) {
        const char* slash = std::strrchr(system_conf, '/');
        if (slash) {
            std::size_t n = static_cast<std::size_t>(slash - system_conf);
            if (n >= sizeof dir) n = sizeof dir - 1;
            std::memcpy(dir, system_conf, n);
            dir[n] = '\0';
        }
    }
    if (dir[0]) std::snprintf(out, cap, "%s/adapters/%s.conf", dir, name);
    else std::snprintf(out, cap, "adapters/%s.conf", name);
}

namespace detail {

// 解析 `key = value`，去掉首尾空白与行尾注释；匹配时把值写进 out。
inline bool adapter_kv(const char* line, const char* key, char* out, std::size_t cap) noexcept {
    if (!line || !key || !out || cap == 0) return false;
    const char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '#' || *p == ';' || *p == '[' || !*p) return false;
    const std::size_t n = std::strlen(key);
    if (std::strncmp(p, key, n) != 0) return false;
    p += n;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '=') return false;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    const char* end = p;
    while (*end && *end != '#' && *end != '\r' && *end != '\n') ++end;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) --end;
    const std::size_t len = static_cast<std::size_t>(end - p);
    if (len == 0) return false;
    const std::size_t w = len < cap - 1 ? len : cap - 1;
    std::memcpy(out, p, w);
    out[w] = '\0';
    return true;
}

} // namespace detail

// 从系统 conf 读 adapter（缺省 ctp）与可选的 adapter_conf。
// conf_out 可为空；未写 adapter_conf 时保持空串，由调用方用
// adapter_conf_default 补齐。
inline void adapter_read_conf(const char* path, char* out, std::size_t cap, char* conf_out = nullptr,
                              std::size_t conf_cap = 0) noexcept {
    if (!out || cap == 0) return;
    std::snprintf(out, cap, "%s", kDefaultAdapter);
    if (conf_out && conf_cap > 0) conf_out[0] = '\0';
    std::FILE* f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (detail::adapter_kv(line, "adapter", out, cap)) continue;
        if (conf_out && conf_cap > 0) detail::adapter_kv(line, "adapter_conf", conf_out, conf_cap);
    }
    std::fclose(f);
}

} // namespace trading
} // namespace pitlane
