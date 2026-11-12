#pragma once

#include "pitlane/util/hint.hpp"

#include <cstdint>

namespace pitlane {

enum class IdKind : uint8_t { Empty = 0, Wait = 1, Live = 2, Final = 3 };

// Fixed-capacity id state table: Wait → Live → Final. Distinct from SlotPool (no body).

template <int Cap>
class IdClaim {
public:
    static_assert(Cap >= 1 && Cap <= 4096, "cap");

    int find(uint64_t id) const noexcept {
        if (id == 0) return -1;
        for (int i = 0; i < Cap; ++i) {
            if (row_[i].kind != IdKind::Empty && row_[i].id == id) return i;
        }
        return -1;
    }

    bool claim(uint64_t id, uint64_t acct) noexcept {
        if (unlikely(id == 0 || find(id) >= 0)) return false;
        for (int i = 0; i < Cap; ++i) {
            if (row_[i].kind != IdKind::Empty) continue;
            row_[i].id = id;
            row_[i].acct = acct;
            row_[i].kind = IdKind::Wait;
            return true;
        }
        return false;
    }

    bool arm_live(uint64_t id) noexcept {
        const int i = find(id);
        if (i < 0 || row_[i].kind != IdKind::Wait) return false;
        row_[i].kind = IdKind::Live;
        return true;
    }

    bool seal(uint64_t id) noexcept {
        const int i = find(id);
        if (i < 0) return false;
        if (row_[i].kind != IdKind::Wait && row_[i].kind != IdKind::Live) return false;
        row_[i].kind = IdKind::Final;
        return true;
    }

    bool drop_wait(uint64_t id) noexcept {
        const int i = find(id);
        if (i < 0 || row_[i].kind != IdKind::Wait) return false;
        row_[i] = Row{};
        return true;
    }

    // 0 live-dup (drop resubmit), 1 final-replay, -1 miss.
    int resubmit(uint64_t id, IdKind& st) const noexcept {
        const int i = find(id);
        if (i < 0) return -1;
        st = row_[i].kind;
        if (row_[i].kind == IdKind::Final) return 1;
        return 0;
    }

    IdKind kind_at(int i) const noexcept {
        if (i < 0 || i >= Cap) return IdKind::Empty;
        return row_[i].kind;
    }

private:
    struct Row {
        uint64_t id{0};
        uint64_t acct{0};
        IdKind kind{IdKind::Empty};
    };

    Row row_[Cap]{};
};

}  // namespace pitlane
