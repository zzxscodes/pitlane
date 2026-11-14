#pragma once

// Ops command channel and audit trail. HTTP threads allocate a sequence and
// push a command; the trading thread takes, validates, and acks. Every ack
// lands in a small per-seq result table the request thread can poll.

#include "pitlane/util/hint.hpp"
#include "pitlane/util/sendjoin.hpp"
#include "pitlane/util/seqpub.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace pitlane {

enum class OpsKind : uint8_t {
    Direct = 1,
    Basket = 2,
    Algo = 3,
    CancelAll = 4,
    Pause = 5,
    Resume = 6,
    Halt = 7,
    ResumeRisk = 8,
    Reload = 9,
    Stop = 10,
};

inline constexpr int kOpsBasketLegs = 8;

struct OpsLeg {
    uint32_t name{0};
    int64_t target{0};
};

struct OpsCommand {
    uint64_t seq{0};
    uint64_t id{0};
    uint8_t kind{0};
    uint8_t side{0};
    uint16_t legs{0};
    uint32_t name{0};
    int64_t qty{0};
    int64_t px{0};
    int64_t horizon_us{0};
    int64_t slice_us{0};
    OpsLeg basket[kOpsBasketLegs]{};
};

struct OpsResult {
    uint64_t seq{0};
    uint64_t id{0};
    int32_t code{0};
    char msg[48]{};
};

enum : int32_t {
    kOpsOk = 0,
    kOpsBadReq = 1,
    kOpsHalted = 2,
    kOpsRisk = 3,
    kOpsBusy = 4,
    kOpsUnknown = 5,
};

struct OpsAuditRec {
    uint64_t seq{0};
    uint64_t id{0};
    int64_t stamp_us{0};
    int32_t code{0};
    uint8_t kind{0};
    uint8_t pad[3]{};
};

inline const char* ops_kind_name(uint8_t kind) noexcept {
    switch (static_cast<OpsKind>(kind)) {
        case OpsKind::Direct: return "direct";
        case OpsKind::Basket: return "basket";
        case OpsKind::Algo: return "algo";
        case OpsKind::CancelAll: return "cancel_all";
        case OpsKind::Pause: return "pause";
        case OpsKind::Resume: return "resume";
        case OpsKind::Halt: return "halt";
        case OpsKind::ResumeRisk: return "resume_risk";
        case OpsKind::Reload: return "reload";
        case OpsKind::Stop: return "stop";
    }
    return "other";
}

inline const char* ops_code_name(int32_t code) noexcept {
    switch (code) {
        case kOpsOk: return "ok";
        case kOpsBadReq: return "bad_request";
        case kOpsHalted: return "blocked";
        case kOpsRisk: return "risk";
        case kOpsBusy: return "busy";
        case kOpsUnknown: return "unknown";
    }
    return "error";
}

template <int Cap = 64>
class OpsAudit {
public:
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "audit capacity");

    void add(const OpsAuditRec& r) noexcept { ring_.publish([&](OpsAuditRec& slot) { slot = r; }); }

    // Copies the most recent records in chronological order. Returns the count.
    int recent(OpsAuditRec* out, int cap) const noexcept {
        if (!out || cap <= 0) return 0;
        const int64_t claimed = ring_.claimed();
        if (claimed <= 0) return 0;
        const int n = claimed < cap ? static_cast<int>(claimed) : cap;
        int got = 0;
        ring_.drain_from(claimed - n, [&](const OpsAuditRec& r) {
            if (got < cap) out[got++] = r;
        });
        return got;
    }

    int64_t claimed() const noexcept { return ring_.claimed(); }

private:
    SeqPublishRing<OpsAuditRec, Cap> ring_{};
};

template <int Cap = 256>
class OpsChannel {
public:
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0, "ops channel capacity");

    uint64_t alloc_seq() noexcept { return next_seq_.fetch_add(1, std::memory_order_relaxed); }

    bool submit(const OpsCommand& c) noexcept { return queue_.try_push(c); }

    bool take(OpsCommand& c) noexcept { return queue_.try_pop(c); }

    void ack(const OpsResult& r, int64_t stamp_us = 0, uint8_t kind = 0) noexcept {
        Slot& s = result_[r.seq & (Cap - 1)];
        s.seq.store(0, std::memory_order_release);
        s.body = r;
        s.seq.store(r.seq, std::memory_order_release);
        if (stamp_us > 0) {
            OpsAuditRec rec{};
            rec.seq = r.seq;
            rec.id = r.id;
            rec.code = r.code;
            rec.kind = kind;
            rec.stamp_us = stamp_us;
            audit_.add(rec);
        }
    }

    bool result(uint64_t seq, OpsResult& out) const noexcept {
        const Slot& s = result_[seq & (Cap - 1)];
        if (s.seq.load(std::memory_order_acquire) != seq) return false;
        out = s.body;
        std::atomic_thread_fence(std::memory_order_acquire);
        return s.seq.load(std::memory_order_relaxed) == seq;
    }

    void audit(const OpsAuditRec& r) noexcept { audit_.add(r); }

    int recent_audit(OpsAuditRec* out, int cap) const noexcept { return audit_.recent(out, cap); }

    int64_t audit_claimed() const noexcept { return audit_.claimed(); }

private:
    struct Slot {
        std::atomic<uint64_t> seq{0};
        OpsResult body{};
    };

    SendJoinRing<OpsCommand, Cap> queue_{};
    Slot result_[Cap]{};
    OpsAudit<64> audit_{};
    std::atomic<uint64_t> next_seq_{1};
};

}  // namespace pitlane
