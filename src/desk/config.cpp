#include "pitlane/desk/config.hpp"

#include "pitlane/host/affinity.hpp"
#include "pitlane/host/hugepage.hpp"
#include "pitlane/host/memlock.hpp"
#include "pitlane/host/priority.hpp"

#include <cstdio>
#include <cstring>

namespace pitlane {
namespace {

bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

char* trim(char* s) noexcept {
    while (*s && is_space(*s))
        ++s;
    if (!*s)
        return s;
    char* e = s + std::strlen(s);
    while (e > s && is_space(e[-1]))
        --e;
    *e = 0;
    return s;
}

bool parse_u8(const char* s, uint8_t& out) noexcept {
    if (!s || !*s)
        return false;
    unsigned long v = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        v = v * 10u + static_cast<unsigned long>(*p - '0');
        if (v > 255ul)
            return false;
    }
    out = static_cast<uint8_t>(v);
    return true;
}

bool parse_i8(const char* s, int8_t& out) noexcept {
    if (!s || !*s)
        return false;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        ++s;
    } else if (*s == '+') {
        ++s;
    }
    long v = 0;
    if (!*s)
        return false;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        v = v * 10 + (*p - '0');
        if (v > 128)
            return false;
    }
    v *= sign;
    if (v < -128 || v > 127)
        return false;
    out = static_cast<int8_t>(v);
    return true;
}

bool parse_u32(const char* s, uint32_t& out) noexcept {
    if (!s || !*s)
        return false;
    unsigned long long v = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        v = v * 10ull + static_cast<unsigned long long>(*p - '0');
        if (v > 0xffffffffull)
            return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

bool parse_i32(const char* s, int& out) noexcept {
    if (!s || !*s)
        return false;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        ++s;
    }
    long long v = 0;
    if (!*s)
        return false;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        v = v * 10 + (*p - '0');
        if (v > 2147483647ll)
            return false;
    }
    v *= sign;
    if (v < -2147483648ll || v > 2147483647ll)
        return false;
    out = static_cast<int>(v);
    return true;
}

bool parse_i64(const char* s, int64_t& out) noexcept {
    if (!s || !*s)
        return false;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        ++s;
    }
    unsigned long long v = 0;
    if (!*s)
        return false;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        if (v > 922337203685477580ull)
            return false;
        v = v * 10ull + static_cast<unsigned long long>(*p - '0');
    }
    if (sign < 0) {
        if (v > 9223372036854775808ull)
            return false;
        out = -static_cast<int64_t>(v);
    } else {
        if (v > 9223372036854775807ull)
            return false;
        out = static_cast<int64_t>(v);
    }
    return true;
}

// Fractional seconds (match.ini delayN) → microseconds. No locale.
bool parse_sec_to_us(const char* s, int64_t& out_us) noexcept {
    if (!s || !*s)
        return false;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        ++s;
    } else if (*s == '+') {
        ++s;
    }
    if (!*s)
        return false;
    unsigned long long whole = 0;
    const char* p = s;
    for (; *p && *p != '.'; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        whole = whole * 10ull + static_cast<unsigned long long>(*p - '0');
        if (whole > 9223372036854ull)
            return false;
    }
    unsigned long long frac = 0;
    int digits = 0;
    if (*p == '.') {
        ++p;
        for (; *p && digits < 6; ++p, ++digits) {
            if (*p < '0' || *p > '9')
                return false;
            frac = frac * 10ull + static_cast<unsigned long long>(*p - '0');
        }
        for (; *p; ++p) {
            if (*p < '0' || *p > '9')
                return false;
        }
        while (digits < 6) {
            frac *= 10ull;
            ++digits;
        }
    }
    const unsigned long long us = whole * 1000000ull + frac;
    if (sign < 0) {
        if (us > 9223372036854775808ull)
            return false;
        out_us = -static_cast<int64_t>(us);
    } else {
        if (us > 9223372036854775807ull)
            return false;
        out_us = static_cast<int64_t>(us);
    }
    return true;
}

// 1 applied, 0 not this key, -1 bad value.
int try_indexed_us(const char* key, const char* prefix, const char* val, int64_t* arr9) noexcept {
    if (!key || !prefix || !arr9)
        return 0;
    const std::size_t n = std::strlen(prefix);
    if (std::strncmp(key, prefix, n) != 0)
        return 0;
    const char* num = key + n;
    if (!*num || num[1] != 0 || *num < '1' || *num > '9')
        return 0;
    return parse_i64(val, arr9[*num - '1']) ? 1 : -1;
}

int try_indexed_sec(const char* key, const char* prefix, const char* val, int64_t* arr9) noexcept {
    if (!key || !prefix || !arr9)
        return 0;
    const std::size_t n = std::strlen(prefix);
    if (std::strncmp(key, prefix, n) != 0)
        return 0;
    const char* num = key + n;
    if (!*num || num[1] != 0 || *num < '1' || *num > '9')
        return 0;
    return parse_sec_to_us(val, arr9[*num - '1']) ? 1 : -1;
}

bool copy_dir(char* dst, std::size_t cap, const char* src) noexcept {
    if (!dst || cap < 2 || !src)
        return false;
    std::size_t n = 0;
    for (; src[n]; ++n) {
        if (n + 1 >= cap)
            return false;
        if (static_cast<unsigned char>(src[n]) < 0x20)
            return false;
    }
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = src[i];
    dst[n] = 0;
    return true;
}

uint32_t floor_pow2(uint32_t v) noexcept {
    if (v < 2)
        return 2;
    uint32_t p = 1;
    while ((p << 1) && (p << 1) <= v)
        p <<= 1;
    return p;
}

uint32_t clamp_pow2(uint32_t want, uint32_t lo, uint32_t hi, bool& changed) noexcept {
    uint32_t v = want;
    if (v < lo) {
        v = lo;
        changed = true;
    }
    if (v > hi) {
        v = hi;
        changed = true;
    }
    const uint32_t p = floor_pow2(v);
    if (p != want)
        changed = true;
    return p;
}

int append_hint(char* hints, int cap, int at, const char* msg) noexcept {
    if (!hints || cap <= 0 || !msg)
        return at;
    const int need = static_cast<int>(std::strlen(msg));
    if (at < 0)
        at = 0;
    if (at >= cap)
        return at;
    if (at + need + 1 >= cap) {
        if (at + 1 < cap)
            hints[at] = 0;
        return at;
    }
    for (int i = 0; i < need; ++i)
        hints[at + i] = msg[i];
    at += need;
    hints[at++] = '\n';
    hints[at] = 0;
    return at;
}

// 1 applied, 0 unknown key (ignored), -1 bad value.
int set_key(PitConf& conf, const char* section, const char* key, const char* val) noexcept {
    if (!key || !val)
        return -1;
    const char* sec = section ? section : "";
    auto ok = [](bool b) { return b ? 1 : -1; };
    if (std::strcmp(sec, "map") == 0 || sec[0] == 0) {
        if (std::strcmp(key, "hugepage") == 0)
            return ok(parse_u8(val, conf.map_hugepage));
        if (std::strcmp(key, "lock") == 0)
            return ok(parse_u8(val, conf.map_lock));
        if (std::strcmp(key, "prefault") == 0)
            return ok(parse_u8(val, conf.map_prefault));
        if (std::strcmp(key, "numa_node") == 0)
            return ok(parse_i8(val, conf.map_numa_node));
        if (std::strcmp(key, "hugepage_dir") == 0)
            return ok(copy_dir(conf.hugepage_dir, sizeof conf.hugepage_dir, val));
        if (sec[0] == 0) {
            /* fall through to other sections when unscoped */
        } else {
            return 0;
        }
    }
    if (std::strcmp(sec, "arena") == 0 || sec[0] == 0) {
        if (std::strcmp(key, "hugepage") == 0 && std::strcmp(sec, "arena") == 0)
            return ok(parse_u8(val, conf.arena_hugepage));
        if (std::strcmp(key, "lock") == 0 && std::strcmp(sec, "arena") == 0)
            return ok(parse_u8(val, conf.arena_lock));
        if (std::strcmp(key, "prefault") == 0 && std::strcmp(sec, "arena") == 0)
            return ok(parse_u8(val, conf.arena_prefault));
        if (std::strcmp(key, "numa_node") == 0 && std::strcmp(sec, "arena") == 0)
            return ok(parse_i8(val, conf.arena_numa_node));
    }
    if (std::strcmp(sec, "desk") == 0 || sec[0] == 0) {
        if (std::strcmp(key, "intent_cap") == 0)
            return ok(parse_u32(val, conf.intent_cap));
        if (std::strcmp(key, "report_cap") == 0)
            return ok(parse_u32(val, conf.report_cap));
        if (std::strcmp(key, "quote_cap") == 0)
            return ok(parse_u32(val, conf.quote_cap));
        if (std::strcmp(key, "algo_cap") == 0)
            return ok(parse_u32(val, conf.algo_cap));
        if (std::strcmp(key, "control_cap") == 0)
            return ok(parse_u32(val, conf.control_cap));
        if (std::strcmp(key, "pulse_cap") == 0)
            return ok(parse_u32(val, conf.pulse_cap));
        if (std::strcmp(key, "print_cap") == 0)
            return ok(parse_u32(val, conf.print_cap));
        if (std::strcmp(key, "query_cap") == 0)
            return ok(parse_u32(val, conf.query_cap));
        if (std::strcmp(key, "pack_req_cap") == 0)
            return ok(parse_u32(val, conf.pack_req_cap));
        if (std::strcmp(key, "pack_rsp_cap") == 0)
            return ok(parse_u32(val, conf.pack_rsp_cap));
        if (std::strcmp(key, "log_cap") == 0)
            return ok(parse_u32(val, conf.log_cap));
        if (std::strcmp(key, "cash_cap") == 0)
            return ok(parse_u32(val, conf.cash_cap));
        if (std::strcmp(key, "hold_cap") == 0)
            return ok(parse_u32(val, conf.hold_cap));
    }
    if (std::strcmp(sec, "broker") == 0 || sec[0] == 0) {
        if (std::strcmp(key, "max_qty") == 0)
            return ok(parse_i64(val, conf.max_qty));
        if (std::strcmp(key, "max_notional") == 0)
            return ok(parse_i64(val, conf.max_notional));
        if (std::strcmp(key, "max_live") == 0)
            return ok(parse_i32(val, conf.max_live));
        if (std::strcmp(key, "ack_limit_us") == 0)
            return ok(parse_i64(val, conf.ack_limit_us));
        if (std::strcmp(key, "life_limit_us") == 0)
            return ok(parse_i64(val, conf.life_limit_us));
        if (std::strcmp(key, "pull_limit_us") == 0)
            return ok(parse_i64(val, conf.pull_limit_us));
        if (std::strcmp(key, "watch_cap") == 0)
            return ok(parse_i32(val, conf.broker_watch_cap));
    }
    if (std::strcmp(sec, "query") == 0 || sec[0] == 0) {
        if (std::strcmp(key, "engines") == 0)
            return ok(parse_u32(val, conf.query_engines));
        if (std::strcmp(key, "accounts") == 0)
            return ok(parse_u32(val, conf.query_accounts));
        if (std::strcmp(key, "queue") == 0)
            return ok(parse_u32(val, conf.query_queue));
        if (std::strcmp(key, "kinds") == 0)
            return ok(parse_u32(val, conf.query_kinds));
    }
    if (std::strcmp(sec, "host") == 0 || sec[0] == 0) {
        if (std::strcmp(key, "cpu") == 0)
            return ok(parse_i32(val, conf.host_cpu));
        if (std::strcmp(key, "policy") == 0)
            return ok(parse_i32(val, conf.host_policy));
        if (std::strcmp(key, "priority") == 0)
            return ok(parse_i32(val, conf.host_priority));
        if (std::strcmp(key, "nice") == 0)
            return ok(parse_i32(val, conf.host_nice));
        if (std::strcmp(key, "set_nice") == 0)
            return ok(parse_u8(val, conf.host_set_nice));
        if (std::strcmp(key, "hugepages") == 0)
            return ok(parse_i64(val, conf.host_hugepages));
        if (std::strcmp(key, "thp_disable") == 0)
            return ok(parse_u8(val, conf.host_thp_disable));
        if (std::strcmp(key, "mlockall") == 0)
            return ok(parse_u8(val, conf.host_mlockall));
        if (std::strcmp(key, "numa_node") == 0)
            return ok(parse_i32(val, conf.host_numa_node));
        if (std::strcmp(key, "irq_list") == 0)
            return ok(copy_dir(conf.host_irq_list, sizeof conf.host_irq_list, val));
    }
    if (std::strcmp(sec, "shed") == 0 || sec[0] == 0) {
        if (std::strcmp(key, "target") == 0)
            return ok(parse_i64(val, conf.shed_target));
        if (std::strcmp(key, "window") == 0)
            return ok(parse_i64(val, conf.shed_window));
    }
    if (std::strcmp(sec, "pace") == 0 || sec[0] == 0) {
        if (std::strcmp(key, "burst") == 0)
            return ok(parse_i64(val, conf.pace_burst));
        if (std::strcmp(key, "rate") == 0)
            return ok(parse_i64(val, conf.pace_rate));
        if (std::strcmp(key, "per") == 0)
            return ok(parse_i64(val, conf.pace_per));
        if (std::strcmp(key, "idle") == 0)
            return ok(parse_i64(val, conf.pace_idle));
        if (std::strcmp(key, "gcra_interval") == 0)
            return ok(parse_i64(val, conf.gcra_interval));
        if (std::strcmp(key, "gcra_tolerance") == 0)
            return ok(parse_i64(val, conf.gcra_tolerance));
    }
    if (std::strcmp(sec, "yard") == 0 || sec[0] == 0) {
        if (std::strcmp(key, "sym_slots") == 0)
            return ok(parse_u32(val, conf.yard_sym_slots));
        if (std::strcmp(key, "quote_lanes") == 0)
            return ok(parse_u32(val, conf.yard_quote_lanes));
        if (std::strcmp(key, "quote_depth") == 0)
            return ok(parse_u32(val, conf.yard_quote_depth));
        if (std::strcmp(key, "order_lanes") == 0)
            return ok(parse_u32(val, conf.yard_order_lanes));
        if (std::strcmp(key, "trade_lanes") == 0)
            return ok(parse_u32(val, conf.yard_trade_lanes));
        if (std::strcmp(key, "rebuild_lanes") == 0)
            return ok(parse_u32(val, conf.yard_rebuild_lanes));
        if (std::strcmp(key, "mix_lanes") == 0)
            return ok(parse_u32(val, conf.yard_mix_lanes));
        if (std::strcmp(key, "order_depth") == 0)
            return ok(parse_u32(val, conf.yard_order_depth));
        if (std::strcmp(key, "trade_depth") == 0)
            return ok(parse_u32(val, conf.yard_trade_depth));
        if (std::strcmp(key, "rebuild_depth") == 0)
            return ok(parse_u32(val, conf.yard_rebuild_depth));
        if (std::strcmp(key, "mix_depth") == 0)
            return ok(parse_u32(val, conf.yard_mix_depth));
        if (std::strcmp(key, "base_info_cap") == 0)
            return ok(parse_u32(val, conf.yard_base_info_cap));
        if (std::strcmp(key, "trade_time_cap") == 0)
            return ok(parse_u32(val, conf.yard_trade_time_cap));
    }
    if (std::strcmp(sec, "study") == 0 || sec[0] == 0) {
        if (std::strcmp(key, "signal_pools") == 0)
            return ok(parse_u32(val, conf.study_signal_pools));
        if (std::strcmp(key, "sample_pools") == 0)
            return ok(parse_u32(val, conf.study_sample_pools));
        if (std::strcmp(key, "factor_pools") == 0)
            return ok(parse_u32(val, conf.study_factor_pools));
        if (std::strcmp(key, "nonstruct_pools") == 0)
            return ok(parse_u32(val, conf.study_nonstruct_pools));
        if (std::strcmp(key, "sample_depth") == 0)
            return ok(parse_u32(val, conf.study_sample_depth));
        if (std::strcmp(key, "factor_depth") == 0)
            return ok(parse_u32(val, conf.study_factor_depth));
        if (std::strcmp(key, "nonstruct_depth") == 0)
            return ok(parse_u32(val, conf.study_nonstruct_depth));
    }
    if (std::strcmp(sec, "venue") == 0 || sec[0] == 0) {
        // µs keys (pitlane native) and sec keys (match.ini delayN / Cancel.delayN).
        if (const int r = try_indexed_us(key, "go_us", val, conf.venue_go_us))
            return r;
        if (const int r = try_indexed_us(key, "pull_us", val, conf.venue_pull_us))
            return r;
        if (const int r = try_indexed_sec(key, "go_sec", val, conf.venue_go_us))
            return r;
        if (const int r = try_indexed_sec(key, "pull_sec", val, conf.venue_pull_us))
            return r;
        // match.ini [Match] delayN / [Cancel] delayN aliases
        if (const int r = try_indexed_sec(key, "delay", val, conf.venue_go_us))
            return r;
        if (const int r = try_indexed_sec(key, "cancel_delay", val, conf.venue_pull_us))
            return r;
        if (std::strcmp(key, "rigid_extra") == 0)
            return ok(parse_i64(val, conf.venue_rigid_extra));
        if (std::strcmp(key, "lot") == 0)
            return ok(parse_i64(val, conf.venue_lot));
        if (std::strcmp(key, "notional_cap") == 0)
            return ok(parse_i64(val, conf.venue_notional_cap));
        if (std::strcmp(key, "rate_cap") == 0)
            return ok(parse_i32(val, conf.venue_rate_cap));
        if (std::strcmp(key, "cancel_pow_x1000") == 0)
            return ok(parse_i64(val, conf.venue_cancel_pow_x1000));
        if (std::strcmp(key, "trade_open") == 0)
            return ok(parse_u8(val, conf.venue_trade_open));
        if (std::strcmp(key, "tape_first") == 0 || std::strcmp(key, "use_ext") == 0)
            return ok(parse_u8(val, conf.venue_tape_first));
        if (std::strcmp(key, "rigid") == 0 || std::strcmp(key, "rigid_rule") == 0)
            return ok(parse_u8(val, conf.venue_rigid));
    }
    return 0;
}

} // namespace

void conf_defaults(PitConf& conf) noexcept {
    conf = PitConf{};
}

bool conf_load_ex(const char* path, PitConf& conf, int* err_line) noexcept {
    if (err_line)
        *err_line = 0;
    conf_defaults(conf);
    if (!path || !path[0])
        return true;
    FILE* f = std::fopen(path, "r");
    if (!f)
        return true; // missing file → defaults
    char line[512];
    char section[64]{};
    int nline = 0;
    while (std::fgets(line, sizeof line, f)) {
        ++nline;
        char* p = trim(line);
        if (!*p || *p == '#' || *p == ';')
            continue;
        if (*p == '[') {
            char* end = std::strchr(p, ']');
            if (!end) {
                if (err_line)
                    *err_line = nline;
                std::fclose(f);
                return false;
            }
            *end = 0;
            char* name = trim(p + 1);
            if (std::strlen(name) >= sizeof section) {
                if (err_line)
                    *err_line = nline;
                std::fclose(f);
                return false;
            }
            std::snprintf(section, sizeof section, "%s", name);
            continue;
        }
        char* eq = std::strchr(p, '=');
        if (!eq) {
            if (err_line)
                *err_line = nline;
            std::fclose(f);
            return false;
        }
        *eq = 0;
        char* key = trim(p);
        char* val = trim(eq + 1);
        if (!*key) {
            if (err_line)
                *err_line = nline;
            std::fclose(f);
            return false;
        }
        if (!*val)
            continue; // empty → keep default
        const int rc = set_key(conf, section, key, val);
        if (rc < 0) {
            if (err_line)
                *err_line = nline;
            std::fclose(f);
            return false;
        }
    }
    std::fclose(f);
    return true;
}

bool conf_load(const char* path, PitConf& conf) noexcept {
    return conf_load_ex(path, conf, nullptr);
}

int conf_normalize(PitConf& conf, char* hints, int hints_cap) noexcept {
    if (hints && hints_cap > 0)
        hints[0] = 0;
    int at = 0;
    int n = 0;
    char msg[192];

    auto fix_ring = [&](uint32_t& field, uint32_t hi, const char* name) {
        bool ch = false;
        const uint32_t before = field;
        field = clamp_pow2(field, 2, hi, ch);
        if (ch) {
            std::snprintf(msg, sizeof msg, "desk.%s: %u -> %u (power-of-two, max %u)", name, before, field, hi);
            at = append_hint(hints, hints_cap, at, msg);
            ++n;
        }
    };

    fix_ring(conf.intent_cap, kIntentCap, "intent_cap");
    fix_ring(conf.report_cap, kReportCap, "report_cap");
    fix_ring(conf.quote_cap, kQuoteCap, "quote_cap");
    fix_ring(conf.algo_cap, kAlgoCap, "algo_cap");
    fix_ring(conf.control_cap, kControlCap, "control_cap");
    fix_ring(conf.pulse_cap, kPulseCap, "pulse_cap");
    fix_ring(conf.print_cap, kPrintCap, "print_cap");
    fix_ring(conf.query_cap, kQueryCap, "query_cap");
    fix_ring(conf.pack_req_cap, kPackReqCap, "pack_req_cap");
    fix_ring(conf.pack_rsp_cap, kPackRspCap, "pack_rsp_cap");
    fix_ring(conf.log_cap, kLogCap, "log_cap");

    auto fix_rows = [&](uint32_t& field, uint32_t hi, const char* name) {
        const uint32_t before = field;
        if (field < 1) {
            field = 1;
        }
        if (field > hi)
            field = hi;
        if (field != before) {
            std::snprintf(msg, sizeof msg, "desk.%s: %u -> %u (max %u)", name, before, field, hi);
            at = append_hint(hints, hints_cap, at, msg);
            ++n;
        }
    };
    fix_rows(conf.cash_cap, kCashCap, "cash_cap");
    fix_rows(conf.hold_cap, kHoldCap, "hold_cap");

    if (conf.map_hugepage > 1) {
        conf.map_hugepage = 1;
        at = append_hint(hints, hints_cap, at, "map.hugepage: clamped to 0|1");
        ++n;
    }
    if (conf.map_lock > 1) {
        conf.map_lock = 1;
        at = append_hint(hints, hints_cap, at, "map.lock: clamped to 0|1");
        ++n;
    }
    if (conf.map_prefault > 1) {
        conf.map_prefault = 1;
        at = append_hint(hints, hints_cap, at, "map.prefault: clamped to 0|1");
        ++n;
    }
    if (conf.hugepage_dir[0] == 0) {
        std::snprintf(conf.hugepage_dir, sizeof conf.hugepage_dir, "/dev/hugepages");
        // silent fill — empty dir is the same as the built-in default
    }

    if (conf.arena_hugepage > 1)
        conf.arena_hugepage = 1;
    if (conf.arena_lock > 1)
        conf.arena_lock = 1;
    if (conf.arena_prefault > 1)
        conf.arena_prefault = 1;

    if (conf.max_qty < 0) {
        conf.max_qty = 0;
        at = append_hint(hints, hints_cap, at, "broker.max_qty: negative -> 0 (off)");
        ++n;
    }
    if (conf.max_notional < 0) {
        conf.max_notional = 0;
        at = append_hint(hints, hints_cap, at, "broker.max_notional: negative -> 0 (off)");
        ++n;
    }
    if (conf.max_live < 0) {
        conf.max_live = 0;
        at = append_hint(hints, hints_cap, at, "broker.max_live: negative -> 0 (off)");
        ++n;
    }
    if (conf.max_live > BrokerWatch::kCap) {
        conf.max_live = BrokerWatch::kCap;
        std::snprintf(msg, sizeof msg, "broker.max_live: clamped to %d", BrokerWatch::kCap);
        at = append_hint(hints, hints_cap, at, msg);
        ++n;
    }
    if (conf.ack_limit_us < 0)
        conf.ack_limit_us = 0;
    if (conf.life_limit_us < 0)
        conf.life_limit_us = 0;
    if (conf.pull_limit_us < 0)
        conf.pull_limit_us = 0;

    if (conf.broker_watch_cap != BrokerWatch::kCap) {
        std::snprintf(msg, sizeof msg, "broker.watch_cap: %d ignored (compile-time %d)", conf.broker_watch_cap,
                      BrokerWatch::kCap);
        conf.broker_watch_cap = BrokerWatch::kCap;
        at = append_hint(hints, hints_cap, at, msg);
        ++n;
    }
    if (conf.query_engines != QueryFold::kEngines || conf.query_accounts != QueryFold::kAccounts ||
        conf.query_queue != QueryFold::kQueue || conf.query_kinds != QueryFold::kKinds) {
        at = append_hint(hints, hints_cap, at,
                         "query.* sizes are compile-time; requested values restored to library defaults");
        conf.query_engines = QueryFold::kEngines;
        conf.query_accounts = QueryFold::kAccounts;
        conf.query_queue = QueryFold::kQueue;
        conf.query_kinds = QueryFold::kKinds;
        ++n;
    }
    if (conf.host_policy < 0 || conf.host_policy > 2) {
        conf.host_policy = 0;
        at = append_hint(hints, hints_cap, at, "host.policy: clamped to 0|1|2");
        ++n;
    }
    if (conf.host_set_nice > 1)
        conf.host_set_nice = 1;
    if (conf.host_thp_disable > 1)
        conf.host_thp_disable = 1;
    if (conf.host_mlockall > 1)
        conf.host_mlockall = 1;
    if (conf.shed_target < 0)
        conf.shed_target = 0;
    if (conf.shed_window < 0)
        conf.shed_window = 0;
    if (conf.pace_per <= 0)
        conf.pace_per = 1;
    if (conf.pace_burst < 0)
        conf.pace_burst = 0;
    if (conf.pace_rate < 0)
        conf.pace_rate = 0;
    if (conf.gcra_interval < 0)
        conf.gcra_interval = 0;
    if (conf.gcra_tolerance < 0)
        conf.gcra_tolerance = 0;

    auto clamp_u32 = [&](uint32_t& field, uint32_t lo, uint32_t hi, const char* name) {
        const uint32_t before = field;
        if (field < lo)
            field = lo;
        if (field > hi)
            field = hi;
        if (field != before) {
            std::snprintf(msg, sizeof msg, "yard.%s: %u -> %u", name, before, field);
            at = append_hint(hints, hints_cap, at, msg);
            ++n;
        }
    };
    clamp_u32(conf.yard_sym_slots, 1, 2000000u, "sym_slots");
    clamp_u32(conf.yard_quote_lanes, 1, 2000000u, "quote_lanes");
    {
        bool ch = false;
        const uint32_t before = conf.yard_quote_depth;
        conf.yard_quote_depth = clamp_pow2(conf.yard_quote_depth, 2, 65536u, ch);
        if (ch) {
            std::snprintf(msg, sizeof msg, "yard.quote_depth: %u -> %u (power-of-two)", before, conf.yard_quote_depth);
            at = append_hint(hints, hints_cap, at, msg);
            ++n;
        }
    }
    clamp_u32(conf.yard_order_lanes, 0, 1024u, "order_lanes");
    clamp_u32(conf.yard_trade_lanes, 0, 1024u, "trade_lanes");
    clamp_u32(conf.yard_rebuild_lanes, 0, 1024u, "rebuild_lanes");
    clamp_u32(conf.yard_mix_lanes, 0, 1024u, "mix_lanes");
    auto clamp_depth = [&](uint32_t& field, const char* name) {
        bool ch = false;
        const uint32_t before = field;
        field = clamp_pow2(field, 2, 1u << 20, ch);
        if (ch) {
            std::snprintf(msg, sizeof msg, "yard.%s: %u -> %u (power-of-two)", name, before, field);
            at = append_hint(hints, hints_cap, at, msg);
            ++n;
        }
    };
    clamp_depth(conf.yard_order_depth, "order_depth");
    clamp_depth(conf.yard_trade_depth, "trade_depth");
    clamp_depth(conf.yard_rebuild_depth, "rebuild_depth");
    clamp_depth(conf.yard_mix_depth, "mix_depth");
    clamp_u32(conf.yard_base_info_cap, 1, 2000000u, "base_info_cap");
    clamp_u32(conf.yard_trade_time_cap, 1, 100000u, "trade_time_cap");

    auto clamp_study = [&](uint32_t& field, uint32_t lo, uint32_t hi, const char* name) {
        const uint32_t before = field;
        if (field < lo) field = lo;
        if (field > hi) field = hi;
        if (field != before) {
            std::snprintf(msg, sizeof msg, "study.%s: %u -> %u", name, before, field);
            at = append_hint(hints, hints_cap, at, msg);
            ++n;
        }
    };
    clamp_study(conf.study_signal_pools, 1, 1000000u, "signal_pools");
    clamp_study(conf.study_sample_pools, 1, 1000000u, "sample_pools");
    clamp_study(conf.study_factor_pools, 1, 1000000u, "factor_pools");
    clamp_study(conf.study_nonstruct_pools, 1, 1000000u, "nonstruct_pools");
    clamp_study(conf.study_sample_depth, 2, 4096u, "sample_depth");
    clamp_study(conf.study_factor_depth, 2, 1024u, "factor_depth");
    clamp_study(conf.study_nonstruct_depth, 2, 1024u, "nonstruct_depth");

    for (int i = 0; i < 9; ++i) {
        if (conf.venue_go_us[i] < 0) {
            conf.venue_go_us[i] = 0;
            at = append_hint(hints, hints_cap, at, "venue.go_us*: negative -> 0");
            ++n;
        }
        if (conf.venue_pull_us[i] < 0) {
            conf.venue_pull_us[i] = 0;
            at = append_hint(hints, hints_cap, at, "venue.pull_us*: negative -> 0");
            ++n;
        }
    }
    if (conf.venue_lot < 1) {
        conf.venue_lot = 1;
        at = append_hint(hints, hints_cap, at, "venue.lot: <1 -> 1");
        ++n;
    }
    if (conf.venue_notional_cap < 0)
        conf.venue_notional_cap = 0;
    if (conf.venue_rate_cap < 0)
        conf.venue_rate_cap = 0;
    if (conf.venue_cancel_pow_x1000 < 1) {
        conf.venue_cancel_pow_x1000 = 1000;
        at = append_hint(hints, hints_cap, at, "venue.cancel_pow_x1000: <1 -> 1000");
        ++n;
    }
    if (conf.venue_trade_open > 1)
        conf.venue_trade_open = 1;
    if (conf.venue_tape_first > 1)
        conf.venue_tape_first = 1;
    if (conf.venue_rigid > 1)
        conf.venue_rigid = 1;
    if (conf.venue_rigid_extra < 0)
        conf.venue_rigid_extra = 0;
    return n;
}

MapOpt conf_map_opt(const PitConf& conf) noexcept {
    MapOpt opt{};
    opt.hugepage = conf.map_hugepage ? 1 : 0;
    opt.lock = conf.map_lock ? 1 : 0;
    opt.prefault = conf.map_prefault ? 1 : 0;
    opt.numa_node = conf.map_numa_node;
    opt.hugepage_dir = conf.hugepage_dir;
    return opt;
}

ArenaOpt conf_arena_opt(const PitConf& conf) noexcept {
    ArenaOpt opt{};
    opt.hugepage = conf.arena_hugepage ? 1 : 0;
    opt.lock = conf.arena_lock ? 1 : 0;
    opt.prefault = conf.arena_prefault ? 1 : 0;
    opt.numa_node = conf.arena_numa_node;
    return opt;
}

RiskCaps conf_risk_caps(const PitConf& conf) noexcept {
    RiskCaps caps{};
    caps.max_qty = conf.max_qty;
    caps.max_notional = conf.max_notional;
    caps.max_live = conf.max_live;
    return caps;
}

YardPlan conf_yard_plan(const PitConf& conf) noexcept {
    YardPlan p{};
    p.sym_slots = conf.yard_sym_slots;
    p.quote_lanes = conf.yard_quote_lanes;
    p.quote_depth = conf.yard_quote_depth;
    p.order_lanes = conf.yard_order_lanes;
    p.trade_lanes = conf.yard_trade_lanes;
    p.rebuild_lanes = conf.yard_rebuild_lanes;
    p.mix_lanes = conf.yard_mix_lanes;
    p.order_depth = conf.yard_order_depth;
    p.trade_depth = conf.yard_trade_depth;
    p.rebuild_depth = conf.yard_rebuild_depth;
    p.mix_depth = conf.yard_mix_depth;
    p.base_info_cap = conf.yard_base_info_cap;
    p.trade_time_cap = conf.yard_trade_time_cap;
    return p;
}

StudyPlan conf_study_plan(const PitConf& conf) noexcept {
    StudyPlan p{};
    p.signal_pools = conf.study_signal_pools;
    p.sample_pools = conf.study_sample_pools;
    p.factor_pools = conf.study_factor_pools;
    p.nonstruct_pools = conf.study_nonstruct_pools;
    p.sample_depth = conf.study_sample_depth;
    p.factor_depth = conf.study_factor_depth;
    p.nonstruct_depth = conf.study_nonstruct_depth;
    return p;
}

VenueCfg conf_venue_cfg(const PitConf& conf) noexcept {
    VenueCfg cfg{};
    for (int i = 0; i < 9; ++i) {
        cfg.lag.go_us[i] = conf.venue_go_us[i];
        cfg.lag.pull_us[i] = conf.venue_pull_us[i];
    }
    cfg.rigid_extra = conf.venue_rigid_extra;
    cfg.lot = conf.venue_lot;
    cfg.cancel_pow = static_cast<double>(conf.venue_cancel_pow_x1000) / 1000.0;
    cfg.rate_cap = conf.venue_rate_cap;
    cfg.notional_cap = conf.venue_notional_cap;
    cfg.trade_open = conf.venue_trade_open ? 1 : 0;
    cfg.tape_first = conf.venue_tape_first ? 1 : 0;
    cfg.rigid = conf.venue_rigid ? 1 : 0;
    return cfg;
}

bool desk_apply_conf(Desk& desk, const PitConf& conf) noexcept {
    if (!desk.intents.tune(conf.intent_cap))
        return false;
    if (!desk.reports.tune(conf.report_cap))
        return false;
    if (!desk.quotes.tune(conf.quote_cap))
        return false;
    if (!desk.algos.tune(conf.algo_cap))
        return false;
    if (!desk.controls.tune(conf.control_cap))
        return false;
    if (!desk.pulses.tune(conf.pulse_cap))
        return false;
    if (!desk.prints.tune(conf.print_cap))
        return false;
    if (!desk.queries.tune(conf.query_cap))
        return false;
    if (!desk.pack_reqs.tune(conf.pack_req_cap))
        return false;
    if (!desk.pack_rsps.tune(conf.pack_rsp_cap))
        return false;
    if (!desk.logs.tune(conf.log_cap))
        return false;
    return true;
}

bool cash_apply_conf(CashBoard& board, const PitConf& conf) noexcept {
    return board.tune(conf.cash_cap);
}

bool hold_apply_conf(HoldBoard& board, const PitConf& conf) noexcept {
    return board.tune(conf.hold_cap);
}

int conf_apply_host(const PitConf& conf, char* hints, int hints_cap) noexcept {
    if (hints && hints_cap > 0)
        hints[0] = 0;
    int at = 0;
    int ok_n = 0;
    char msg[192];
    if (conf.host_cpu >= 0) {
        if (pin_thread(conf.host_cpu)) {
            ++ok_n;
        } else {
            std::snprintf(msg, sizeof msg, "host.cpu=%d: pin_thread failed", conf.host_cpu);
            at = append_hint(hints, hints_cap, at, msg);
        }
    }
    if (conf.host_policy == 1) {
        if (set_fifo_thread(conf.host_priority))
            ++ok_n;
        else
            at = append_hint(hints, hints_cap, at, "host.policy=FIFO: set_fifo_thread failed");
    } else if (conf.host_policy == 2) {
        if (set_rr_thread(conf.host_priority))
            ++ok_n;
        else
            at = append_hint(hints, hints_cap, at, "host.policy=RR: set_rr_thread failed");
    }
    if (conf.host_set_nice) {
        if (set_nice(conf.host_nice))
            ++ok_n;
        else
            at = append_hint(hints, hints_cap, at, "host.nice: set_nice failed");
    }
    if (conf.host_hugepages >= 0) {
        if (hugepage_count_ensure(static_cast<long>(conf.host_hugepages)))
            ++ok_n;
        else
            at = append_hint(hints, hints_cap, at, "host.hugepages: ensure failed");
    }
    if (conf.host_thp_disable) {
        if (thp_disable())
            ++ok_n;
        else
            at = append_hint(hints, hints_cap, at, "host.thp_disable: failed");
    }
    if (conf.host_mlockall) {
        if (mem_lock_all())
            ++ok_n;
        else
            at = append_hint(hints, hints_cap, at, "host.mlockall: failed");
    }
    (void)conf.host_numa_node;
    (void)conf.host_irq_list; // IRQ needs an irq number; list alone is documentary in conf
    if (conf.host_irq_list[0]) {
        at = append_hint(hints, hints_cap, at, "host.irq_list: stored; call irq_affinity_list_set with irq id");
    }
    return ok_n;
}

} // namespace pitlane
