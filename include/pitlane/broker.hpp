#pragma once

#include "pitlane/types.hpp"

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
    int live() const noexcept { return live_; }

private:
    struct Sent {
        uint64_t tag{0};
        int64_t sent_us{0};
        int64_t pull_us{0};
        uint8_t acked{0};
        uint8_t used{0};
    };
    Sent row_[kCap]{};
    int live_{0};
};

class QueryFold {
public:
    static constexpr uint32_t kEngines = 4;
    static constexpr uint32_t kAccounts = 32;
    static constexpr uint32_t kQueue = 16;

    bool push(uint32_t engine, uint32_t acct) noexcept;
    void fold() noexcept;
    int pending(uint32_t* out, int cap) const noexcept;
    void clear_acct(uint32_t acct) noexcept;

private:
    struct Lane {
        uint32_t q[kQueue]{};
        uint32_t head{0};
        uint32_t tail{0};
    };
    Lane lane_[kEngines]{};
    uint8_t wait_[kEngines][kAccounts]{};
};

bool acct_visible(const AcctLink* rows, int n, const char* trader, const char* counter, const char* account) noexcept;
bool reply_route(const char* counter, char* out, int cap) noexcept;

}  // namespace pitlane
