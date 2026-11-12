#pragma once

#include "pitlane/desk/types.hpp"

#include <cstdint>

namespace pitlane {

struct RiskCaps {
    int64_t max_qty{0};
    int64_t max_notional{0};
    int max_live{0};
};

struct AcctLink {
    char trader[32]{};
    char counter[24]{};
    char account[32]{};
    uint8_t enabled{0};
};

class BrokerWatch {
public:
    static constexpr int kCap = 128;

    Why screen(const Intent& in, const RiskCaps& caps) const noexcept;
    bool remember(const Intent& in, int64_t now_us) noexcept;
    bool ack(uint64_t tag) noexcept;
    bool mark_pull(uint64_t tag, int64_t now_us) noexcept;
    int expire(int64_t now_us, int64_t ack_limit_us, int64_t life_limit_us, int64_t pull_limit_us, uint64_t* tags,
               int cap) noexcept;
    // List due tags without dropping rows. Short output returns -1 and writes nothing.
    int due(int64_t now_us, int64_t ack_limit_us, int64_t life_limit_us, int64_t pull_limit_us, uint64_t* tags,
            int cap) const noexcept;
    int live() const noexcept { return live_; }

private:
    struct Sent {
        uint64_t tag{0};
        int64_t sent_us{0};
        int64_t pull_us{0};
        uint8_t acked{0};
        uint8_t used{0};
    };

    static bool row_due(const Sent& s, int64_t now_us, int64_t ack_limit_us, int64_t life_limit_us,
                        int64_t pull_limit_us) noexcept;

    Sent row_[kCap]{};
    int live_{0};
};

struct QueryWant {
    uint32_t acct{0};
    uint8_t kind{0};
    uint64_t page{0};
};

class QueryFold {
public:
    // Sized for trading center strategy engine count+trading engine count (9+15) with headroom (not Desk mmap).
    static constexpr uint32_t kEngines = 32;
    static constexpr uint32_t kAccounts = 64;
    static constexpr uint32_t kQueue = 16;
    static constexpr uint32_t kKinds = 8;

    bool push(uint32_t engine, uint32_t acct) noexcept;
    bool push(uint32_t engine, uint32_t acct, uint8_t kind, uint64_t page) noexcept;
    void fold() noexcept;
    int pending(uint32_t* out, int cap) const noexcept;
    // Distinct (acct, kind) rows with optional page token. Short output returns -1.
    int pending_kind(QueryWant* out, int cap) const noexcept;
    void clear_acct(uint32_t acct) noexcept;
    void clear_want(uint32_t acct, uint8_t kind) noexcept;

private:
    struct Lane {
        uint32_t q[kQueue]{};
        uint8_t kind[kQueue]{};
        uint64_t page[kQueue]{};
        uint32_t head{0};
        uint32_t tail{0};
    };
    Lane lane_[kEngines]{};
    uint8_t wait_[kEngines][kAccounts][kKinds]{};
    uint64_t page_[kEngines][kAccounts][kKinds]{};
};

bool acct_visible(const AcctLink* rows, int n, const char* trader, const char* counter, const char* account) noexcept;
bool reply_route(const char* counter, char* out, int cap) noexcept;

}  // namespace pitlane
