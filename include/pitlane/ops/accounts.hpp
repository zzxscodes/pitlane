#pragma once

// Fund accounts. One row per trading account; each gateway binds to one of
// them. The numeric id is the file order plus one so the existing strategy
// path (acct = 1) keeps working, while the web console uses the string id.
//
// Format: account = <id> <display> <class> <ccy> <cash> <test|live>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pitlane {

enum AccEnv : uint8_t { kEnvTest = 0, kEnvLive = 1 };

inline const char* acc_env_name(uint8_t env) noexcept { return env == kEnvLive ? "live" : "test"; }

inline bool acc_env_from_name(const char* s, uint8_t& env) noexcept {
    if (!s) return false;
    if (std::strcmp(s, "test") == 0) {
        env = kEnvTest;
        return true;
    }
    if (std::strcmp(s, "live") == 0) {
        env = kEnvLive;
        return true;
    }
    return false;
}

struct AccountSpec {
    char id[24]{};
    char name[32]{};
    char klass[12]{};
    char ccy[8]{};
    int64_t cash{0};
    uint8_t env{kEnvTest};
    uint32_t num{0};  // 1-based, stable: file order + 1
};

class AccountRegistry {
public:
    static constexpr int kMaxAccounts = 8;

    // Ignores comments, blank lines, and unknown keys; duplicate ids keep the
    // first row. Returns false only when the file cannot be opened.
    bool load(const char* path) noexcept {
        n_ = 0;
        std::FILE* f = std::fopen(path, "r");
        if (!f) return false;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p || *p == '#' || *p == '\n' || *p == '\r') continue;
            char* eq = std::strchr(p, '=');
            if (!eq) continue;
            *eq = 0;
            char* key_end = eq;
            while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t')) --key_end;
            *key_end = 0;
            if (std::strcmp(p, "account") != 0) continue;
            char id[24]{}, name[32]{}, klass[12]{}, ccy[8]{}, env[8]{};
            long long cash = 0;
            if (std::sscanf(eq + 1, " %23s %31s %11s %7s %lld %7s", id, name, klass, ccy, &cash, env) != 6) continue;
            uint8_t e = kEnvTest;
            if (!acc_env_from_name(env, e)) continue;
            if (!id[0] || !name[0] || !klass[0] || !ccy[0] || cash < 0) continue;
            if (n_ >= kMaxAccounts || index_of(id) >= 0) continue;
            AccountSpec s{};
            std::snprintf(s.id, sizeof s.id, "%s", id);
            std::snprintf(s.name, sizeof s.name, "%s", name);
            std::snprintf(s.klass, sizeof s.klass, "%s", klass);
            std::snprintf(s.ccy, sizeof s.ccy, "%s", ccy);
            s.cash = cash;
            s.env = e;
            s.num = static_cast<uint32_t>(n_ + 1);
            specs_[n_++] = s;
        }
        std::fclose(f);
        return true;
    }

    int count() const noexcept { return n_; }
    const AccountSpec& spec(int i) const noexcept { return specs_[i]; }

    int index_of(const char* id) const noexcept {
        if (!id) return -1;
        for (int i = 0; i < n_; ++i) {
            if (std::strcmp(specs_[i].id, id) == 0) return i;
        }
        return -1;
    }

    int index_of_num(uint32_t num) const noexcept {
        for (int i = 0; i < n_; ++i) {
            if (specs_[i].num == num) return i;
        }
        return -1;
    }

    // Numeric token resolves directly; otherwise it is matched against the id.
    // Returns 0 when the token matches nothing.
    uint32_t resolve(const char* token) const noexcept {
        if (!token || !*token) return 0;
        bool digits = true;
        for (const char* p = token; *p; ++p) {
            if (*p < '0' || *p > '9') {
                digits = false;
                break;
            }
        }
        if (digits) return static_cast<uint32_t>(std::strtoul(token, nullptr, 10));
        const int i = index_of(token);
        return i < 0 ? 0 : specs_[i].num;
    }

    int count_env(uint8_t env) const noexcept {
        int n = 0;
        for (int i = 0; i < n_; ++i) {
            if (specs_[i].env == env) ++n;
        }
        return n;
    }

    bool inject(const AccountSpec& s) noexcept {
        if (n_ >= kMaxAccounts || index_of(s.id) >= 0) return false;
        specs_[n_] = s;
        specs_[n_].num = static_cast<uint32_t>(n_ + 1);
        ++n_;
        return true;
    }

private:
    AccountSpec specs_[kMaxAccounts]{};
    int n_{0};
};

}  // namespace pitlane
