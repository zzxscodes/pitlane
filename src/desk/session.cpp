#include "pitlane/desk/session.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pitlane {
namespace {

char* trim(char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    if (!*s) return s;
    char* e = s + std::strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    *e = 0;
    return s;
}

bool parse_i64(const char* s, int64_t& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const long long v = std::strtoll(s, &end, 10);
    if (!end || *end) return false;
    out = static_cast<int64_t>(v);
    return true;
}

bool parse_i32(const char* s, int& out) {
    int64_t v = 0;
    if (!parse_i64(s, v) || v < INT32_MIN || v > INT32_MAX) return false;
    out = static_cast<int>(v);
    return true;
}

void split_csv(const char* s, std::vector<std::string>& out) {
    out.clear();
    if (!s) return;
    std::string cur;
    for (const char* p = s;; ++p) {
        if (*p == ',' || *p == 0) {
            while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) cur.pop_back();
            std::size_t i = 0;
            while (i < cur.size() && (cur[i] == ' ' || cur[i] == '\t')) ++i;
            if (i < cur.size()) out.emplace_back(cur.substr(i));
            cur.clear();
            if (!*p) break;
            continue;
        }
        cur.push_back(*p);
    }
}

const char* env_first(const char* a, const char* b) {
    if (const char* p = std::getenv(a)) {
        if (*p) return p;
    }
    if (const char* p = std::getenv(b)) {
        if (*p) return p;
    }
    return nullptr;
}

} // namespace

bool load_account_conf(const char* path, Account& acct, StratCfg& strat, std::string& err) {
    err.clear();
    FILE* f = std::fopen(path, "r");
    if (!f) {
        err = "open failed";
        return false;
    }
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        char* p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;
        char* eq = std::strchr(p, '=');
        if (!eq) {
            err = "bad line";
            std::fclose(f);
            return false;
        }
        *eq = 0;
        char* key = trim(p);
        char* val = trim(eq + 1);
        if (std::strcmp(key, "broker_id") == 0) acct.broker_id = val;
        else if (std::strcmp(key, "investor_id") == 0) acct.investor_id = val;
        else if (std::strcmp(key, "password") == 0) acct.password = val;
        else if (std::strcmp(key, "app_id") == 0) acct.app_id = val;
        else if (std::strcmp(key, "auth_code") == 0) acct.auth_code = val;
        else if (std::strcmp(key, "md_front") == 0) acct.md_front = val;
        else if (std::strcmp(key, "td_front") == 0) acct.td_front = val;
        else if (std::strcmp(key, "instruments") == 0) split_csv(val, acct.instruments);
        else if (std::strcmp(key, "work_dir") == 0) acct.work_dir = val;
        else if (std::strcmp(key, "flow_md_dir") == 0) acct.flow_md_dir = val;
        else if (std::strcmp(key, "flow_td_dir") == 0) acct.flow_td_dir = val;
        else if (std::strcmp(key, "desk_path") == 0) acct.desk_path = val;
        else if (std::strcmp(key, "log_path") == 0) acct.log_path = val;
        else if (std::strcmp(key, "host_cpu") == 0) {
            if (!parse_i32(val, acct.host_cpu)) {
                err = "host_cpu";
                std::fclose(f);
                return false;
            }
        } else if (std::strcmp(key, "buy_px_x10000") == 0) {
            if (!parse_i64(val, strat.buy_px_x10000)) {
                err = "buy_px";
                std::fclose(f);
                return false;
            }
        } else if (std::strcmp(key, "max_spread_x10000") == 0) {
            if (!parse_i64(val, strat.max_spread_x10000)) {
                err = "max_spread";
                std::fclose(f);
                return false;
            }
        } else if (std::strcmp(key, "order_qty") == 0) {
            if (!parse_i64(val, strat.order_qty)) {
                err = "order_qty";
                std::fclose(f);
                return false;
            }
        } else if (std::strcmp(key, "min_order_gap_us") == 0) {
            if (!parse_i64(val, strat.min_order_gap_us)) {
                err = "gap";
                std::fclose(f);
                return false;
            }
        } else if (std::strcmp(key, "max_live_orders") == 0) {
            if (!parse_i32(val, strat.max_live_orders)) {
                err = "max_live";
                std::fclose(f);
                return false;
            }
        } else if (std::strcmp(key, "t2t_samples") == 0) {
            if (!parse_i32(val, strat.t2t_samples)) {
                err = "t2t";
                std::fclose(f);
                return false;
            }
        } else if (std::strcmp(key, "connect_timeout_s") == 0) {
            if (!parse_i32(val, strat.connect_timeout_s)) {
                err = "connect_timeout_s";
                std::fclose(f);
                return false;
            }
        }
    }
    std::fclose(f);
    return true;
}

void apply_account_env(Account& acct) {
    if (const char* p = env_first("PITLANE_PASSWORD", "SIMNOW_PASSWORD")) acct.password = p;
    if (const char* p = env_first("PITLANE_INVESTOR", "SIMNOW_INVESTOR")) acct.investor_id = p;
    if (const char* p = env_first("PITLANE_WORK_DIR", "SIMNOW_WORK_DIR")) acct.work_dir = p;
}

void resolve_runtime_paths(Account& acct) {
    if (acct.work_dir.empty()) acct.work_dir = "./run";
    auto join = [](const std::string& base, const char* leaf) {
        if (!base.empty() && base.back() == '/') return base + leaf;
        return base + "/" + leaf;
    };
    if (acct.flow_md_dir.empty()) acct.flow_md_dir = join(acct.work_dir, "flow_md");
    if (acct.flow_td_dir.empty()) acct.flow_td_dir = join(acct.work_dir, "flow_td");
    if (acct.desk_path.empty()) acct.desk_path = join(acct.work_dir, "desk.bin");
    if (acct.log_path.empty()) acct.log_path = join(acct.work_dir, "wire.log");
    if (!acct.flow_md_dir.empty() && acct.flow_md_dir.back() != '/') acct.flow_md_dir.push_back('/');
    if (!acct.flow_td_dir.empty() && acct.flow_td_dir.back() != '/') acct.flow_td_dir.push_back('/');
}

} // namespace pitlane
