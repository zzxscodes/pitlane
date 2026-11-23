#pragma once

// CTP 柜台自己的配置。系统 conf 只写 adapter = ctp 和 adapter_conf（可省），
// 连接参数、账号与前置地址都留在本适配器的 conf 文件里，核心库不认识这些键。
//
// 默认路径：系统 conf 同目录下的 adapters/ctp.conf。示例见
// conf/adapters/ctp.conf.example。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace pitlane {
namespace trading {

struct CtpCfg {
    std::string broker_id;
    std::string investor_id;
    std::string password;
    std::string new_password; // 仅在柜台返回 141（密码过期）时使用，改密成功后清空
    std::string app_id;
    std::string auth_code;
    std::string md_front;
    std::string td_front;
};

namespace detail {

inline char* ctp_trim(char* s) noexcept {
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) return s;
    char* e = s + std::strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    *e = 0;
    return s;
}

} // namespace detail

// 读 CTP 配置。文件打不开时返回 false 并写 err；未知键忽略。
inline bool ctp_conf_load(const char* path, CtpCfg& cfg, std::string& err) noexcept {
    err.clear();
    std::FILE* f = path && *path ? std::fopen(path, "rb") : nullptr;
    if (!f) {
        err = "open failed";
        return false;
    }
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        char* p = detail::ctp_trim(line);
        if (!*p || *p == '#' || *p == ';' || *p == '[') continue;
        char* eq = std::strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = detail::ctp_trim(p);
        char* val = detail::ctp_trim(eq + 1);
        if (std::strcmp(key, "broker_id") == 0) cfg.broker_id = val;
        else if (std::strcmp(key, "investor_id") == 0) cfg.investor_id = val;
        else if (std::strcmp(key, "password") == 0) cfg.password = val;
        else if (std::strcmp(key, "new_password") == 0) cfg.new_password = val;
        else if (std::strcmp(key, "app_id") == 0) cfg.app_id = val;
        else if (std::strcmp(key, "auth_code") == 0) cfg.auth_code = val;
        else if (std::strcmp(key, "md_front") == 0) cfg.md_front = val;
        else if (std::strcmp(key, "td_front") == 0) cfg.td_front = val;
    }
    std::fclose(f);
    return true;
}

// PITLANE_CTP_PASSWORD / PITLANE_CTP_INVESTOR，兼容 SIMNOW_* 别名。
inline void ctp_apply_env(CtpCfg& cfg) noexcept {
    if (const char* p = std::getenv("PITLANE_CTP_PASSWORD")) {
        if (*p) cfg.password = p;
    } else if (const char* p = std::getenv("SIMNOW_PASSWORD")) {
        if (*p) cfg.password = p;
    }
    if (const char* p = std::getenv("PITLANE_CTP_INVESTOR")) {
        if (*p) cfg.investor_id = p;
    } else if (const char* p = std::getenv("SIMNOW_INVESTOR")) {
        if (*p) cfg.investor_id = p;
    }
}

// 返回第一个缺失的必填键，全部就绪时返回 nullptr。
inline const char* ctp_missing(const CtpCfg& cfg) noexcept {
    if (cfg.investor_id.empty()) return "investor_id";
    if (cfg.password.empty()) return "password";
    if (cfg.md_front.empty()) return "md_front";
    if (cfg.td_front.empty()) return "td_front";
    return nullptr;
}

} // namespace trading
} // namespace pitlane
