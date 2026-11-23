#pragma once

// Gateway registry. A deployment declares every market-data and trading
// gateway in one file; the hub owns their state and the web console starts or
// stops them through the same audited command channel as orders. The registry
// itself is adapter-agnostic: the example supplies start/stop callbacks.

#include "pitlane/ops/accounts.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

enum GwKind : uint8_t { kGwMd = 0, kGwTd = 1 };

enum GwState : uint8_t { kGwStopped = 0, kGwStarting = 1, kGwRunning = 2, kGwFailed = 3 };

inline const char* gw_kind_name(uint8_t kind) noexcept { return kind == kGwTd ? "td" : "md"; }

inline const char* gw_state_name(uint8_t state) noexcept {
    switch (state) {
        case kGwStopped: return "stopped";
        case kGwStarting: return "starting";
        case kGwRunning: return "running";
        case kGwFailed: return "failed";
    }
    return "stopped";
}

struct GwSpec {
    char id[24]{};
    char adapter[16]{};
    char acct[24]{};   // account token from conf (string id or number)
    char env[8]{"test"};
    uint8_t env_bit{kEnvTest};
    uint32_t acct_num{0};
    uint8_t kind{kGwMd};
    bool auto_start{false};
    bool available{false};
};

struct GwStatus {
    uint8_t state{kGwStopped};
    char note[48]{};
    int64_t since_us{0};
};

class GatewayRegistry {
public:
    static constexpr int kMaxGateways = 16;

    // gateway = <id> <md|td> <adapter> <acct> <auto 0|1> [test|live]
    // Venue endpoints and credentials belong to the adapter's own conf.
    bool load(const char* path) noexcept {
        n_ = 0;
        std::FILE* f = std::fopen(path, "r");
        if (!f) return false;
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p || *p == '#' || *p == '\n' || *p == '\r') continue;
            char* eq = std::strchr(p, '=');
            if (!eq) continue;
            *eq = 0;
            char* key = p;
            char* key_end = eq;
            while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t')) --key_end;
            *key_end = 0;
            if (std::strcmp(key, "gateway") != 0) continue;
            char id[24]{}, kind[8]{}, adapter[16]{}, acct[24]{};
            int auto_start = 0;
            int used = 0;
            if (std::sscanf(eq + 1, " %23s %7s %15s %23s %d %n", id, kind, adapter, acct, &auto_start, &used) < 5)
                continue;
            GwSpec s{};
            std::snprintf(s.id, sizeof s.id, "%s", id);
            std::snprintf(s.adapter, sizeof s.adapter, "%s", adapter);
            std::snprintf(s.acct, sizeof s.acct, "%s", acct);
            s.kind = std::strcmp(kind, "td") == 0 ? kGwTd : kGwMd;
            s.auto_start = auto_start != 0;
            const char* rest = eq + 1 + used;
            while (*rest == ' ' || *rest == '\t') ++rest;
            uint8_t env = kEnvTest;
            if (token_is(rest, "test")) {
                env = kEnvTest;
                rest += 4;
            } else if (token_is(rest, "live")) {
                env = kEnvLive;
                rest += 4;
            }
            s.env_bit = env;
            std::snprintf(s.env, sizeof s.env, "%s", acc_env_name(env));
            if (n_ < kMaxGateways && index_of(s.id) < 0) specs_[n_++] = s;
        }
        std::fclose(f);
        return n_ > 0;
    }

    int count() const noexcept { return n_; }
    const GwSpec& spec(int i) const noexcept { return specs_[i]; }
    const GwStatus& status(int i) const noexcept { return status_[i]; }

    int index_of(const char* id) const noexcept {
        if (!id) return -1;
        for (int i = 0; i < n_; ++i) {
            if (std::strcmp(specs_[i].id, id) == 0) return i;
        }
        return -1;
    }

    void mark(int i, uint8_t state, const char* note, int64_t now_us) noexcept {
        if (i < 0 || i >= n_) return;
        status_[i].state = state;
        status_[i].since_us = now_us;
        std::snprintf(status_[i].note, sizeof status_[i].note, "%s", note ? note : "");
    }

    void set_available(int i, bool on) noexcept {
        if (i >= 0 && i < n_) specs_[i].available = on;
    }

    int running_count() const noexcept {
        int n = 0;
        for (int i = 0; i < n_; ++i) {
            if (status_[i].state == kGwRunning) ++n;
        }
        return n;
    }

    int failed_count() const noexcept {
        int n = 0;
        for (int i = 0; i < n_; ++i) {
            if (status_[i].state == kGwFailed) ++n;
        }
        return n;
    }

    int count_env(uint8_t env) const noexcept {
        int n = 0;
        for (int i = 0; i < n_; ++i) {
            if (specs_[i].env_bit == env) ++n;
        }
        return n;
    }

    void set_account_num(int i, uint32_t num) noexcept {
        if (i >= 0 && i < n_) specs_[i].acct_num = num;
    }

private:
    static bool token_is(const char* p, const char* tok) noexcept {
        const std::size_t n = std::strlen(tok);
        if (std::strncmp(p, tok, n) != 0) return false;
        const char c = p[n];
        return c == 0 || c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    GwSpec specs_[kMaxGateways]{};
    GwStatus status_[kMaxGateways]{};
    int n_{0};
};

} // namespace pitlane
