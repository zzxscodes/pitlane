#pragma once

#include <cstdint>

namespace pitlane {

// Integer token bucket and theoretical arrival time. The time unit belongs to the caller.
// A full table refuses a new key. An idle slot may be reused after the idle time.

struct TokenPot {
    int64_t tokens{0};
    int64_t burst{0};
    int64_t rate{0};
    int64_t per{1};
    int64_t last{0};
    int64_t frac{0};
    uint8_t on{0};
};

struct GcraPot {
    int64_t tat{0};
    int64_t interval{0};
    int64_t tolerance{0};
    uint8_t on{0};
};

inline bool token_open(TokenPot& pot, int64_t burst, int64_t rate, int64_t per, int64_t now) noexcept {
    if (burst <= 0 || rate <= 0 || per <= 0) return false;
    pot.tokens = burst;
    pot.burst = burst;
    pot.rate = rate;
    pot.per = per;
    pot.last = now;
    pot.frac = 0;
    pot.on = 1;
    return true;
}

inline void token_refill(TokenPot& pot, int64_t now) noexcept {
    if (!pot.on || now < pot.last) return;
    if (pot.last < 0 && now > INT64_MAX + pot.last) {
        pot.tokens = pot.burst;
        pot.frac = 0;
        pot.last = now;
        return;
    }
    const int64_t step = now - pot.last;
    if (pot.frac > INT64_MAX - step) {
        pot.tokens = pot.burst;
        pot.frac = 0;
        pot.last = now;
        return;
    }
    const int64_t elapsed = step + pot.frac;
    const int64_t batches = elapsed / pot.per;
    if (batches > 0) {
        if (pot.rate > INT64_MAX / batches || pot.tokens > pot.burst - 1) {
            pot.tokens = pot.burst;
        } else {
            const int64_t add = batches * pot.rate;
            if (add >= pot.burst || pot.tokens > pot.burst - add) pot.tokens = pot.burst;
            else pot.tokens += add;
        }
    }
    pot.frac = elapsed % pot.per;
    pot.last = now;
}

inline bool token_take(TokenPot& pot, int64_t now, int64_t n) noexcept {
    if (!pot.on || n <= 0 || n > pot.burst) return false;
    token_refill(pot, now);
    if (pot.tokens < n) return false;
    pot.tokens -= n;
    return true;
}

inline bool gcra_open(GcraPot& pot, int64_t interval, int64_t tolerance) noexcept {
    if (interval <= 0 || tolerance < 0) return false;
    pot.tat = 0;
    pot.interval = interval;
    pot.tolerance = tolerance;
    pot.on = 1;
    return true;
}

inline bool gcra_allow(GcraPot& pot, int64_t now) noexcept {
    if (!pot.on) return false;
    const int64_t threshold = pot.tat - pot.tolerance;
    if (now < threshold) return false;
    const int64_t base = pot.tat > now ? pot.tat : now;
    if (base > INT64_MAX - pot.interval) return false;
    pot.tat = base + pot.interval;
    return true;
}

template <int Cap>
class TokenBook {
public:
    static_assert(Cap >= 1 && Cap <= 64, "tokens");

    bool open(int64_t burst, int64_t rate, int64_t per, int64_t idle) noexcept {
        if (burst <= 0 || rate <= 0 || per <= 0 || idle < 0) return false;
        burst_ = burst;
        rate_ = rate;
        per_ = per;
        idle_ = idle;
        on_ = 1;
        return true;
    }

    bool take(uint64_t key, int64_t now, int64_t n) noexcept {
        if (!on_ || key == 0) return false;
        int at = find(key, now);
        if (at < 0) return false;
        Slot& slot = slot_[at];
        if (slot.key != key) {
            TokenPot fresh;
            if (!token_open(fresh, burst_, rate_, per_, now) || !token_take(fresh, now, n)) return false;
            slot.pot = fresh;
            slot.key = key;
            slot.last_used = now;
            return true;
        }
        const bool ok = token_take(slot.pot, now, n);
        if (ok || (n > 0 && n <= burst_)) slot.last_used = now;
        return ok;
    }

private:
    struct Slot {
        uint64_t key{0};
        int64_t last_used{0};
        TokenPot pot{};
    };

    int find(uint64_t key, int64_t now) noexcept {
        int empty = -1;
        int stale = -1;
        for (int i = 0; i < Cap; ++i) {
            if (slot_[i].key == key) return i;
            if (slot_[i].key == 0) {
                if (empty < 0) empty = i;
                continue;
            }
            if (idle_ > 0 && now >= slot_[i].last_used && now - slot_[i].last_used >= idle_ && stale < 0) stale = i;
        }
        if (empty >= 0) return empty;
        return stale;
    }

    Slot slot_[Cap]{};
    int64_t burst_{0};
    int64_t rate_{0};
    int64_t per_{1};
    int64_t idle_{0};
    uint8_t on_{0};
};

template <int Cap>
class GcraBook {
public:
    static_assert(Cap >= 1 && Cap <= 64, "gcra");

    bool open(int64_t interval, int64_t tolerance, int64_t idle) noexcept {
        if (interval <= 0 || tolerance < 0 || idle < 0) return false;
        interval_ = interval;
        tolerance_ = tolerance;
        idle_ = idle;
        on_ = 1;
        return true;
    }

    bool allow(uint64_t key, int64_t now) noexcept {
        if (!on_ || key == 0) return false;
        int at = -1;
        int empty = -1;
        int stale = -1;
        for (int i = 0; i < Cap; ++i) {
            if (slot_[i].key == key) {
                at = i;
                break;
            }
            if (slot_[i].key == 0) {
                if (empty < 0) empty = i;
            } else if (idle_ > 0 && now >= slot_[i].last_used && now - slot_[i].last_used >= idle_ && stale < 0) {
                stale = i;
            }
        }
        if (at < 0) at = empty >= 0 ? empty : stale;
        if (at < 0) return false;
        Slot& slot = slot_[at];
        if (slot.key != key) {
            GcraPot fresh;
            if (!gcra_open(fresh, interval_, tolerance_) || !gcra_allow(fresh, now)) return false;
            slot.pot = fresh;
            slot.key = key;
            slot.last_used = now;
            return true;
        }
        const bool ok = gcra_allow(slot.pot, now);
        slot.last_used = now;
        return ok;
    }

private:
    struct Slot {
        uint64_t key{0};
        int64_t last_used{0};
        GcraPot pot{};
    };

    Slot slot_[Cap]{};
    int64_t interval_{0};
    int64_t tolerance_{0};
    int64_t idle_{0};
    uint8_t on_{0};
};

}  // namespace pitlane
