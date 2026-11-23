#pragma once

// Accounts, roles, and browser sessions for the ops console. The store is a
// flat file owned by the hub; passwords are PBKDF2-HMAC-SHA256 verifiers so no
// plaintext ever reaches disk. Everything here runs on the HTTP thread, which
// is single-threaded, so no locking is needed.

#include "pitlane/ops/sha256.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace pitlane {

enum OpsRole : uint8_t {
    kRoleViewer = 0,
    kRoleTrader = 1,
    kRoleRisk = 2,
    kRoleOps = 3,
    kRoleAdmin = 4,
    kRoleCount = 5,
};

constexpr uint32_t kPermMarket = 1u << 0;
constexpr uint32_t kPermTrade = 1u << 1;
constexpr uint32_t kPermRiskView = 1u << 2;
constexpr uint32_t kPermRiskControl = 1u << 3;
constexpr uint32_t kPermOpsView = 1u << 4;
constexpr uint32_t kPermGateway = 1u << 5;
constexpr uint32_t kPermUsers = 1u << 6;
constexpr uint32_t kPermAudit = 1u << 7;
constexpr uint32_t kPermClear = 1u << 8;

inline const char* ops_role_name(uint8_t role) noexcept {
    switch (role) {
        case kRoleViewer: return "viewer";
        case kRoleTrader: return "trader";
        case kRoleRisk: return "risk";
        case kRoleOps: return "ops";
        case kRoleAdmin: return "admin";
    }
    return "viewer";
}

inline bool ops_role_from_name(const char* s, uint8_t& role) noexcept {
    if (!s) return false;
    for (uint8_t r = 0; r < kRoleCount; ++r) {
        if (std::strcmp(s, ops_role_name(r)) == 0) {
            role = r;
            return true;
        }
    }
    return false;
}

// Viewer can watch; traders add orders; risk adds controls and the audit trail;
// ops adds gateways; admin adds account management.
inline uint32_t ops_role_perms(uint8_t role) noexcept {
    switch (role) {
        case kRoleViewer: return kPermMarket | kPermRiskView | kPermOpsView;
        case kRoleTrader: return kPermMarket | kPermRiskView | kPermOpsView | kPermTrade;
        case kRoleRisk: return kPermMarket | kPermRiskView | kPermOpsView | kPermRiskControl | kPermAudit | kPermClear;
        case kRoleOps: return kPermMarket | kPermRiskView | kPermOpsView | kPermGateway | kPermAudit;
        case kRoleAdmin: return 0xffffffffu;
    }
    return 0;
}

inline bool ops_has(uint32_t perms, uint32_t need) noexcept { return (perms & need) == need; }

constexpr uint32_t kAuthIterations = 10000;

struct OpsUser {
    char name[32]{};
    char salt[33]{};  // 16 random bytes as hex, plus terminator
    char hash[65]{};  // 32-byte verifier as hex
    uint8_t role{kRoleViewer};
    bool enabled{true};
};

inline bool ops_user_name_ok(const char* s) noexcept {
    if (!s || !*s) return false;
    std::size_t n = 0;
    for (; s[n]; ++n) {
        const char c = s[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.';
        if (!ok || n >= sizeof(OpsUser::name) - 1) return false;
    }
    return n >= 1;
}

inline bool ops_password_ok(const char* s) noexcept {
    if (!s) return false;
    const std::size_t n = std::strlen(s);
    return n >= 6 && n <= 64;
}

inline bool ops_random_bytes(uint8_t* out, std::size_t n) noexcept {
    const int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::read(fd, out + got, n - got);
        if (r <= 0) {
            ::close(fd);
            return false;
        }
        got += static_cast<std::size_t>(r);
    }
    ::close(fd);
    return true;
}

inline bool ops_hash_password(const char* pass, const char* salt_hex, char* out_hex) noexcept {
    uint8_t salt[16];
    if (!hex_decode(salt_hex, std::strlen(salt_hex), salt, sizeof salt) || std::strlen(salt_hex) != 32) return false;
    uint8_t digest[32];
    pbkdf2_sha256(pass, std::strlen(pass), salt, sizeof salt, kAuthIterations, digest);
    hex_encode(digest, sizeof digest, out_hex);
    std::memset(digest, 0, sizeof digest);
    return true;
}

class OpsUserStore {
public:
    static constexpr int kMaxUsers = 32;

    int count() const noexcept { return n_; }
    const OpsUser& at(int i) const noexcept { return users_[i]; }

    int find(const char* name) const noexcept {
        if (!name) return -1;
        for (int i = 0; i < n_; ++i) {
            if (std::strcmp(users_[i].name, name) == 0) return i;
        }
        return -1;
    }

    bool add(const char* name, const char* password, uint8_t role, bool persist, const char* path) noexcept {
        if (!ops_user_name_ok(name) || !ops_password_ok(password) || role >= kRoleCount) return false;
        if (n_ >= kMaxUsers || find(name) >= 0) return false;
        OpsUser u{};
        std::snprintf(u.name, sizeof u.name, "%s", name);
        uint8_t salt[16];
        if (!ops_random_bytes(salt, sizeof salt)) return false;
        hex_encode(salt, sizeof salt, u.salt);
        if (!ops_hash_password(password, u.salt, u.hash)) return false;
        u.role = role;
        u.enabled = true;
        users_[n_++] = u;
        if (persist && path) return save(path);
        return true;
    }

    bool remove(const char* name, bool persist, const char* path) noexcept {
        const int idx = find(name);
        if (idx < 0) return false;
        for (int i = idx; i + 1 < n_; ++i) users_[i] = users_[i + 1];
        --n_;
        if (persist && path) return save(path);
        return true;
    }

    bool set_role(const char* name, uint8_t role, bool persist, const char* path) noexcept {
        const int idx = find(name);
        if (idx < 0 || role >= kRoleCount) return false;
        users_[idx].role = role;
        if (persist && path) return save(path);
        return true;
    }

    bool set_password(const char* name, const char* password, bool persist, const char* path) noexcept {
        const int idx = find(name);
        if (idx < 0 || !ops_password_ok(password)) return false;
        uint8_t salt[16];
        if (!ops_random_bytes(salt, sizeof salt)) return false;
        hex_encode(salt, sizeof salt, users_[idx].salt);
        if (!ops_hash_password(password, users_[idx].salt, users_[idx].hash)) return false;
        if (persist && path) return save(path);
        return true;
    }

    bool set_enabled(const char* name, bool on, bool persist, const char* path) noexcept {
        const int idx = find(name);
        if (idx < 0) return false;
        users_[idx].enabled = on;
        if (persist && path) return save(path);
        return true;
    }

    bool verify(const char* name, const char* password, uint8_t& role, uint32_t& perms) const noexcept {
        const int idx = find(name);
        if (idx < 0 || !users_[idx].enabled || !password) return false;
        char hash[65];
        if (!ops_hash_password(password, users_[idx].salt, hash)) return false;
        uint8_t a[32], b[32];
        const bool shape = hex_decode(hash, 64, a, sizeof a) && hex_decode(users_[idx].hash, 64, b, sizeof b);
        const bool ok = shape && secure_equal(a, b, sizeof a);
        std::memset(a, 0, sizeof a);
        std::memset(b, 0, sizeof b);
        if (!ok) return false;
        role = users_[idx].role;
        perms = ops_role_perms(role);
        return true;
    }

    // Format: one `user = name role salt hash` per line; other keys ignored so
    // the ops config can share the file.
    bool load(const char* path) noexcept {
        std::FILE* f = std::fopen(path, "r");
        if (!f) return false;
        n_ = 0;
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
            char* val = eq + 1;
            if (std::strcmp(key, "user") != 0) continue;
            char name[32]{}, role[16]{}, salt[33]{}, hash[65]{};
            if (std::sscanf(val, " %31s %15s %32s %64s", name, role, salt, hash) != 4) continue;
            uint8_t r = 0;
            if (!ops_role_from_name(role, r) || !ops_user_name_ok(name) || n_ >= kMaxUsers) continue;
            if (std::strlen(salt) != 32 || std::strlen(hash) != 64) continue;
            OpsUser u{};
            std::snprintf(u.name, sizeof u.name, "%s", name);
            std::snprintf(u.salt, sizeof u.salt, "%s", salt);
            std::snprintf(u.hash, sizeof u.hash, "%s", hash);
            u.role = r;
            users_[n_++] = u;
        }
        std::fclose(f);
        return n_ > 0;
    }

    bool save(const char* path) const noexcept {
        std::FILE* f = std::fopen(path, "w");
        if (!f) return false;
        std::fprintf(f, "# pitlane ops accounts (pbkdf2-sha256, %u rounds)\n", kAuthIterations);
        std::fprintf(f, "# user = <name> <role:viewer|trader|risk|ops|admin> <salt> <hash>\n");
        for (int i = 0; i < n_; ++i) {
            std::fprintf(f, "user = %s %s %s %s\n", users_[i].name, ops_role_name(users_[i].role), users_[i].salt,
                         users_[i].hash);
        }
        const bool ok = std::fflush(f) == 0;
        if (std::fclose(f) != 0 || !ok) return false;
        return true;
    }

private:
    OpsUser users_[kMaxUsers]{};
    int n_{0};
};

struct OpsSession {
    char token[65]{};
    char user[32]{};
    uint8_t role{kRoleViewer};
    uint8_t mode{0};  // 0 test, 1 live
    uint32_t perms{0};
    int64_t expires_us{0};
    bool active{false};
};

class OpsSessionStore {
public:
    static constexpr int kMaxSessions = 64;
    static constexpr int64_t kTtlUs = 8ll * 3600ll * 1000000ll;

    int create(const OpsUser& u, int64_t now_us, char* token_out, uint8_t mode = 0) noexcept {
        int slot = -1;
        for (int i = 0; i < kMaxSessions; ++i) {
            if (!sessions_[i].active) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            // Reap the oldest expired session; otherwise refuse.
            int64_t oldest = 0;
            for (int i = 0; i < kMaxSessions; ++i) {
                if (!sessions_[i].active) continue;
                if (sessions_[i].expires_us <= now_us) {
                    slot = i;
                    break;
                }
                if (slot < 0 || sessions_[i].expires_us < oldest) {
                    slot = i;
                    oldest = sessions_[i].expires_us;
                }
            }
            if (slot < 0) return -1;
        }
        uint8_t raw[32];
        if (!ops_random_bytes(raw, sizeof raw)) return -1;
        OpsSession& s = sessions_[slot];
        s = OpsSession{};
        hex_encode(raw, sizeof raw, s.token);
        std::memset(raw, 0, sizeof raw);
        std::snprintf(s.user, sizeof s.user, "%s", u.name);
        s.role = u.role;
        s.mode = mode;
        s.perms = ops_role_perms(u.role);
        s.expires_us = now_us + kTtlUs;
        s.active = true;
        if (token_out) std::memcpy(token_out, s.token, sizeof s.token);
        return slot;
    }

    int find(const char* token, int64_t now_us) const noexcept {
        if (!token || std::strlen(token) != 64) return -1;
        for (int i = 0; i < kMaxSessions; ++i) {
            if (!sessions_[i].active) continue;
            if (std::strcmp(sessions_[i].token, token) != 0) continue;
            return sessions_[i].expires_us > now_us ? i : -1;
        }
        return -1;
    }

    void drop(const char* token) noexcept {
        const int idx = find(token, 0);
        if (idx >= 0) sessions_[idx].active = false;
    }

    void drop_user(const char* user) noexcept {
        for (int i = 0; i < kMaxSessions; ++i) {
            if (sessions_[i].active && std::strcmp(sessions_[i].user, user) == 0) sessions_[i].active = false;
        }
    }

    const OpsSession& at(int i) const noexcept { return sessions_[i]; }

    int active_count(int64_t now_us) const noexcept {
        int n = 0;
        for (int i = 0; i < kMaxSessions; ++i) {
            if (sessions_[i].active && sessions_[i].expires_us > now_us) ++n;
        }
        return n;
    }

    void gc(int64_t now_us) noexcept {
        for (int i = 0; i < kMaxSessions; ++i) {
            if (sessions_[i].active && sessions_[i].expires_us <= now_us) sessions_[i].active = false;
        }
    }

private:
    OpsSession sessions_[kMaxSessions]{};
};

// Extracts a bearer token from an Authorization header or the pitlane_token
// cookie. Returns false when neither carries a plausible 64-char token.
inline bool ops_token_from(const char* auth, const char* cookie, char* out, std::size_t cap) noexcept {
    if (auth && std::strncmp(auth, "Bearer ", 7) == 0) {
        const char* tok = auth + 7;
        if (std::strlen(tok) == 64 && cap >= 65) {
            std::memcpy(out, tok, 65);
            return true;
        }
    }
    if (cookie && *cookie) {
        const char* p = std::strstr(cookie, "pitlane_token=");
        if (p) {
            p += 14;
            std::size_t n = 0;
            while (p[n] && p[n] != ';' && n < 64) ++n;
            if (n == 64 && cap >= 65) {
                std::memcpy(out, p, 64);
                out[64] = 0;
                return true;
            }
        }
    }
    return false;
}

} // namespace pitlane
