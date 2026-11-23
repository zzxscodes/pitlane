#pragma once

// Alarm ring shared by the trading thread and the HTTP console. Producers
// claim a slot, fill it, and publish; late readers may lose old entries.

#include "pitlane/ops/accounts.hpp"
#include "pitlane/util/seqpub.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace pitlane {

enum AlarmKind : uint8_t {
    kAlarmReject = 1,
    kAlarmRisk = 2,
    kAlarmStale = 3,
    kAlarmJump = 4,
    kAlarmLink = 5,
    kAlarmGateway = 6,
    kAlarmOrder = 7,
    kAlarmClearing = 8,
};

enum AlarmSev : uint8_t { kAlarmInfo = 0, kAlarmWarn = 1, kAlarmCritical = 2 };

inline const char* alarm_kind_name(uint8_t kind) noexcept {
    switch (kind) {
        case kAlarmReject: return "reject";
        case kAlarmRisk: return "risk";
        case kAlarmStale: return "stale";
        case kAlarmJump: return "jump";
        case kAlarmLink: return "link";
        case kAlarmGateway: return "gateway";
        case kAlarmOrder: return "order";
        case kAlarmClearing: return "clearing";
        default: return "info";
    }
}

inline const char* alarm_sev_name(uint8_t sev) noexcept {
    switch (sev) {
        case kAlarmCritical: return "critical";
        case kAlarmWarn: return "warn";
        default: return "info";
    }
}

struct Alarm {
    int64_t seq{0};
    int64_t stamp_us{0};
    uint8_t kind{kAlarmInfo};
    uint8_t sev{kAlarmInfo};
    uint8_t env{kEnvTest};
    uint8_t pad{0};
    char text[96]{};
};

class AlarmBoard {
public:
    static constexpr int kCap = 32;

    int64_t push(uint8_t kind, uint8_t sev, uint8_t env, int64_t now_us, const char* fmt, ...) noexcept {
        Alarm a{};
        a.kind = kind;
        a.sev = sev;
        a.env = env;
        a.stamp_us = now_us;
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(a.text, sizeof a.text, fmt, ap);
        va_end(ap);
        int64_t seq = 0;
        ring_.publish_claimed([&](Alarm& slot, int64_t h) {
            a.seq = h + 1;
            seq = a.seq;
            slot = a;
        });
        return seq;
    }

    // Copies up to cap entries newer than `since`, keeping the newest ones.
    int recent(Alarm* out, int cap, int64_t since, uint8_t env_filter, bool filter_env) const noexcept {
        if (!out || cap <= 0) return 0;
        int n = 0;
        ring_.drain_from(since, [&](const Alarm& a) {
            if (filter_env && a.env != env_filter) return;
            if (n < cap) {
                out[n++] = a;
            } else {
                for (int i = 1; i < cap; ++i) out[i - 1] = out[i];
                out[cap - 1] = a;
            }
        });
        return n;
    }

    int64_t claimed() const noexcept { return ring_.claimed(); }

private:
    mutable SeqPublishRing<Alarm, kCap> ring_{};
};

}  // namespace pitlane
