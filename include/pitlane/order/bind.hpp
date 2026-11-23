#pragma once

#include <cstdint>
#include <cstring>

namespace pitlane {

// Local id on one side, counter id on the other. A full table refuses the new link.
struct CounterKey {
    uint64_t num{0};
    uint32_t kind{0};  // 0 number, 1 text
    char text[24]{};
    char api[16]{};
};

inline bool same_counter(const CounterKey& a, const CounterKey& b) noexcept {
    if (a.kind != b.kind) return false;
    if (std::memcmp(a.api, b.api, sizeof(a.api)) != 0) return false;
    if (a.kind == 1) return std::memcmp(a.text, b.text, sizeof(a.text)) == 0;
    return a.num == b.num;
}

inline uint32_t mix_counter(const CounterKey& key) noexcept {
    uint64_t x = key.num ^ (static_cast<uint64_t>(key.kind) << 32);
    for (int i = 0; i < 24; ++i) x = x * 131u + static_cast<uint8_t>(key.text[i]);
    for (int i = 0; i < 16; ++i) x = x * 131u + static_cast<uint8_t>(key.api[i]);
    return static_cast<uint32_t>(x ^ (x >> 32));
}

template <int Cap>
class IdBind {
public:
    static_assert(Cap >= 2 && Cap <= 1024 && (Cap & (Cap - 1)) == 0, "power of two");

    int size() const noexcept { return live_; }

    bool put(const CounterKey& key, uint64_t local) noexcept {
        if (local == 0) return false;
        int tomb = -1;
        uint32_t i = mix_counter(key) & (Cap - 1);
        for (int n = 0; n < Cap; ++n) {
            Slot& slot = slot_[i];
            if (slot.state == 0) {
                write(tomb >= 0 ? tomb : static_cast<int>(i), key, local);
                return true;
            }
            if (slot.state == 3 && tomb < 0) tomb = static_cast<int>(i);
            else if (slot.state != 3 && same_counter(slot.key, key)) return false;
            i = (i + 1) & (Cap - 1);
        }
        if (tomb < 0) return false;
        write(tomb, key, local);
        return true;
    }

    bool confirm(const CounterKey& key) noexcept {
        Slot* slot = find_mut(key);
        if (!slot || slot->state != 1) return false;
        slot->state = 2;
        return true;
    }

    bool find(const CounterKey& key, uint64_t* local) const noexcept {
        const Slot* slot = find_const(key);
        if (!slot) return false;
        if (local) *local = slot->local;
        return true;
    }

    bool find_local(uint64_t local, CounterKey* key) const noexcept {
        if (local == 0) return false;
        for (int i = 0; i < Cap; ++i) {
            if (slot_[i].state == 1 || slot_[i].state == 2) {
                if (slot_[i].local == local) {
                    if (key) *key = slot_[i].key;
                    return true;
                }
            }
        }
        return false;
    }

    bool erase(const CounterKey& key) noexcept {
        Slot* slot = find_mut(key);
        if (!slot) return false;
        slot->state = 3;
        slot->local = 0;
        --live_;
        return true;
    }

private:
    struct Slot {
        CounterKey key{};
        uint64_t local{0};
        uint8_t state{0};  // 0 empty, 1 pending, 2 linked, 3 tomb
    };

    void write(int at, const CounterKey& key, uint64_t local) noexcept {
        slot_[at].key = key;
        slot_[at].local = local;
        slot_[at].state = 1;
        ++live_;
    }

    Slot* find_mut(const CounterKey& key) noexcept {
        uint32_t i = mix_counter(key) & (Cap - 1);
        for (int n = 0; n < Cap; ++n) {
            Slot& slot = slot_[i];
            if (slot.state == 0) return nullptr;
            if (slot.state != 3 && same_counter(slot.key, key)) return &slot;
            i = (i + 1) & (Cap - 1);
        }
        return nullptr;
    }

    const Slot* find_const(const CounterKey& key) const noexcept {
        uint32_t i = mix_counter(key) & (Cap - 1);
        for (int n = 0; n < Cap; ++n) {
            const Slot& slot = slot_[i];
            if (slot.state == 0) return nullptr;
            if (slot.state != 3 && same_counter(slot.key, key)) return &slot;
            i = (i + 1) & (Cap - 1);
        }
        return nullptr;
    }

    Slot slot_[Cap]{};
    int live_{0};
};

enum class BindPhase : uint8_t { None = 0, Sent = 1, Acked = 2, Partial = 3, Filled = 4, Cancelled = 5 };

struct BindOrder {
    BindPhase phase{BindPhase::None};
    int64_t filled_qty{0};
    int64_t filled_amt{0};
    BindPhase early_phase{BindPhase::None};
    int64_t early_qty{0};
    int64_t early_amt{0};
};

inline void bind_fill(BindOrder& order, int64_t qty, int64_t px) noexcept {
    if (qty <= 0 || px <= 0) return;
    const int64_t amt = qty > INT64_MAX / px ? INT64_MAX : qty * px;
    if (order.phase == BindPhase::None || order.phase == BindPhase::Sent || order.phase == BindPhase::Acked) {
        if (order.early_qty > INT64_MAX - qty) order.early_qty = INT64_MAX;
        else order.early_qty += qty;
        if (order.early_amt > INT64_MAX - amt) order.early_amt = INT64_MAX;
        else order.early_amt += amt;
        return;
    }
    if (order.filled_qty > INT64_MAX - qty) order.filled_qty = INT64_MAX;
    else order.filled_qty += qty;
    if (order.filled_amt > INT64_MAX - amt) order.filled_amt = INT64_MAX;
    else order.filled_amt += amt;
}

inline void bind_status(BindOrder& order, BindPhase phase) noexcept {
    order.phase = phase;
    if (order.early_qty <= 0) return;
    order.filled_qty = order.early_qty;
    order.filled_amt = order.early_amt;
    order.early_qty = 0;
    order.early_amt = 0;
    if (order.phase == BindPhase::Acked) order.phase = BindPhase::Partial;
}

inline void bind_final(BindOrder& order, BindPhase phase, int64_t venue_left, int64_t local_left) noexcept {
    if (phase != BindPhase::Cancelled && phase != BindPhase::Filled) return;
    if (local_left > venue_left) {
        order.early_phase = phase;
        return;
    }
    order.phase = phase;
}

inline void bind_catchup(BindOrder& order, int64_t qty, int64_t px, int64_t left) noexcept {
    if (qty > 0 && px > 0) {
        const int64_t amt = qty > INT64_MAX / px ? INT64_MAX : qty * px;
        if (order.filled_qty > INT64_MAX - qty) order.filled_qty = INT64_MAX;
        else order.filled_qty += qty;
        if (order.filled_amt > INT64_MAX - amt) order.filled_amt = INT64_MAX;
        else order.filled_amt += amt;
    }
    if (order.early_phase != BindPhase::None && left == 0) {
        order.phase = order.early_phase;
        order.early_phase = BindPhase::None;
    }
}

struct QuoteHold {
    int64_t px{0};
    int64_t qty{0};
    int64_t filled{0};
    uint8_t cancel_out{0};
};

inline void quote_fill(QuoteHold& quote, int64_t qty, int64_t px) noexcept {
    if (qty <= 0) return;
    if (quote.filled > INT64_MAX - qty) quote.filled = INT64_MAX;
    else quote.filled += qty;
    quote.px = px;
    if (quote.qty > qty) quote.qty -= qty;
    else quote.qty = 0;
}

inline bool quote_repost(const QuoteHold& quote, int64_t px) noexcept {
    return !(quote.cancel_out && quote.px == px);
}

}  // namespace pitlane
