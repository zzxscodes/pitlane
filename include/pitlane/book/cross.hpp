#pragma once

#include "pitlane/desk/types.hpp"

#include <cstdint>

namespace pitlane {

struct CrossIn {
    uint64_t tag{0};
    uint32_t acct{0};
    int64_t px{0};
    int64_t qty{0};
    uint8_t side{0};
    uint8_t style{0};
};

struct Deal {
    uint64_t taker{0};
    uint64_t maker{0};
    int64_t px{0};
    int64_t qty{0};
    uint8_t taker_side{0};
};

struct CallIn {
    uint64_t tag{0};
    int64_t px{0};
    int64_t qty{0};
    uint8_t side{0};
};

struct CallOut {
    uint64_t buy_tag{0};
    uint64_t sell_tag{0};
    int64_t px{0};
    int64_t qty{0};
};

// Price-time book. Buys rank high to low, sells low to high, FIFO inside a price.
// A fill is priced at the resting order. An empty price is removed before the next lookup.
// Passive orders that would cross are rejected. Same-account crossing is rejected
// before any quantity is taken when the opposing size cannot complete a fill-or-kill,
// and a day or immediate order stops when the next resting order is the same account.
class CrossBook {
public:
    CrossBook();
    void reset() noexcept;

    int submit(const CrossIn& in, Deal* out, int cap, int* why) noexcept;
    bool withdraw(uint64_t tag, int64_t* leaves_out) noexcept;

    bool top_bid(int64_t* px, int64_t* qty) const noexcept;
    bool top_ask(int64_t* px, int64_t* qty) const noexcept;
    uint32_t resting() const noexcept { return live_; }

private:
    struct Node {
        uint32_t prev;
        uint32_t next;
        uint32_t level;
        uint64_t tag;
        uint32_t acct;
        int64_t px;
        int64_t leaves;
        uint8_t side;
    };
    struct Level {
        uint32_t head;
        uint32_t tail;
        uint32_t worse;
        uint32_t better;
        int64_t px;
        int64_t sum;
        uint8_t side;
    };

    static constexpr uint32_t kNodes = 2048;
    static constexpr uint32_t kLevels = 512;
    static constexpr uint32_t kHash = 1024;

    Node nodes_[kNodes]{};
    Level levels_[kLevels]{};
    uint32_t free_node_{0};
    uint32_t free_level_{0};
    uint32_t best_bid_{0};
    uint32_t best_ask_{0};
    uint32_t live_{0};

    uint32_t px_slot_[kHash]{};
    uint8_t px_state_[kHash]{};  // 0 empty, 1 live, 2 tomb
    uint64_t tag_key_[kHash * 2]{};
    uint32_t tag_node_[kHash * 2]{};
    uint8_t tag_state_[kHash * 2]{};

    uint32_t alloc_node() noexcept;
    void recycle_node(uint32_t id) noexcept;
    uint32_t alloc_level() noexcept;
    void recycle_level(uint32_t id) noexcept;

    uint32_t* best_of(uint8_t side) noexcept;
    const uint32_t* best_of(uint8_t side) const noexcept;

    uint32_t find_level(uint8_t side, int64_t px) const noexcept;
    uint32_t find_tag(uint64_t tag) const noexcept;
    void index_level(uint32_t id) noexcept;
    void drop_level_index(uint8_t side, int64_t px) noexcept;
    void index_tag(uint64_t tag, uint32_t node) noexcept;
    void drop_tag(uint64_t tag) noexcept;

    void link_level(uint32_t id) noexcept;
    void unlink_level(uint32_t id) noexcept;
    void push_tail(uint32_t level, uint32_t node) noexcept;
    void unlink_node(uint32_t node) noexcept;

    int64_t clean_qty(uint8_t side, int64_t px, uint32_t acct) const noexcept;
    bool next_is_self(uint8_t side, int64_t px, uint32_t acct) const noexcept;
};

// Uniform call price: maximise matched volume. tie_higher picks the higher price
// when two prices match the same volume; otherwise the price closer to ref_px wins.
// Fills follow input order inside the chosen price (earlier row is earlier in time).
int64_t auction_clear(const CallIn* in, int n, int64_t ref_px, int tie_higher, CallOut* out, int cap,
                      int64_t* matched);

}  // namespace pitlane
