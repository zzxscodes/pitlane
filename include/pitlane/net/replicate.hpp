#pragma once

#include <cstdint>
#include <type_traits>

// Fixed-capacity replicated log. Indexes are 1-based. A follower appends the
// leader entries in order; a leader tracks the match index of three nodes and
// advances the commit index at the configured quorum. No allocation.

namespace pitlane {

enum class ReplicaRole : uint8_t { Follower = 1, Leader = 2 };

template <class T, uint32_t Cap = 4096>
class ReplicaLog {
public:
    static_assert(Cap >= 8, "Cap must be at least 8");
    static_assert(std::is_trivially_copyable<T>::value, "entry must be trivially copyable");

    int64_t last_index() const noexcept { return count_; }
    int64_t last_term() const noexcept { return term_at(count_); }
    int64_t commit_index() const noexcept { return commit_; }
    int64_t apply_index() const noexcept { return applied_; }
    ReplicaRole role() const noexcept { return role_; }
    int64_t term() const noexcept { return term_; }

    void become_leader(int64_t term, int quorum) noexcept {
        if (quorum < 1) quorum = 1;
        if (quorum > 3) quorum = 3;
        role_ = ReplicaRole::Leader;
        term_ = term;
        quorum_ = quorum;
        match_[0] = count_;
        match_[1] = 0;
        match_[2] = 0;
    }

    bool leader_append(const T& value, int64_t now_us) noexcept {
        (void)now_us;
        if (role_ != ReplicaRole::Leader) return false;
        if (count_ >= static_cast<int>(Cap)) return false;
        ++count_;
        const int slot = slot_of(count_);
        terms_[slot] = term_;
        index_[slot] = count_;
        value_[slot] = value;
        match_[0] = count_;
        return true;
    }

    int follower_append(int64_t term, int64_t prev_index, int64_t prev_term, const T* entries, int n,
                        int64_t leader_commit) noexcept {
        if (role_ != ReplicaRole::Follower) return -1;
        if (term < term_) return -1;
        if (prev_index < 0 || prev_index > count_) return -1;
        if (n < 0) return -1;
        if (n > 0 && !entries) return -1;
        if (prev_index > 0 && term_at(prev_index) != prev_term) return -1;

        const int64_t want = prev_index + static_cast<int64_t>(n);
        if (want > static_cast<int64_t>(Cap)) return -1;

        if (prev_index < count_ && (n == 0 || term_at(prev_index + 1) != term)) count_ = static_cast<int>(prev_index);

        int have = count_ > prev_index ? static_cast<int>(count_ - prev_index) : 0;
        if (have > n) have = n;
        for (int i = have; i < n; ++i) {
            const int64_t idx = prev_index + 1 + static_cast<int64_t>(i);
            const int slot = slot_of(idx);
            terms_[slot] = term;
            index_[slot] = idx;
            value_[slot] = entries[i];
        }
        if (want > count_) count_ = static_cast<int>(want);

        if (term > term_) term_ = term;
        const int64_t reached = leader_commit < static_cast<int64_t>(count_) ? leader_commit : static_cast<int64_t>(count_);
        if (reached > commit_) commit_ = reached;
        return n;
    }

    void match(int node, int64_t index) noexcept {
        if (role_ != ReplicaRole::Leader) return;
        if (node < 0 || node > 2) return;
        if (index < 0) index = 0;
        if (index > static_cast<int64_t>(count_)) index = static_cast<int64_t>(count_);
        match_[node] = index;
    }

    int64_t commit_quorum() noexcept {
        if (role_ != ReplicaRole::Leader) return commit_;
        int q = quorum_;
        if (q < 1) q = 1;
        if (q > 3) q = 3;

        int64_t m[3] = {match_[0], match_[1], match_[2]};
        if (m[0] < m[1]) {
            const int64_t tmp = m[0];
            m[0] = m[1];
            m[1] = tmp;
        }
        if (m[1] < m[2]) {
            const int64_t tmp = m[1];
            m[1] = m[2];
            m[2] = tmp;
        }
        if (m[0] < m[1]) {
            const int64_t tmp = m[0];
            m[0] = m[1];
            m[1] = tmp;
        }

        int64_t reached = m[q - 1];
        if (reached < 0) reached = 0;
        if (reached > static_cast<int64_t>(count_)) reached = static_cast<int64_t>(count_);
        if (reached > commit_) commit_ = reached;
        return commit_;
    }

    int apply(T* out, int cap) noexcept {
        if (!out || cap <= 0) return 0;
        const int64_t upto = commit_ < static_cast<int64_t>(count_) ? commit_ : static_cast<int64_t>(count_);
        if (upto <= applied_) return 0;
        const int64_t avail = upto - applied_;
        if (avail > static_cast<int64_t>(cap)) return 0;
        for (int i = 0; i < static_cast<int>(avail); ++i) {
            const int64_t idx = applied_ + 1 + static_cast<int64_t>(i);
            out[i] = value_[slot_of(idx)];
        }
        applied_ = upto;
        return static_cast<int>(avail);
    }

private:
    int slot_of(int64_t index) const noexcept {
        return static_cast<int>((index - 1) % static_cast<int64_t>(Cap));
    }

    int64_t term_at(int64_t index) const noexcept {
        if (index <= 0 || index > static_cast<int64_t>(count_)) return 0;
        return terms_[slot_of(index)];
    }

    int64_t terms_[Cap];
    int64_t index_[Cap];
    T value_[Cap];
    int count_{0};
    ReplicaRole role_{ReplicaRole::Follower};
    int64_t term_{0};
    int64_t commit_{0};
    int64_t applied_{0};
    int64_t match_[3]{};
    int quorum_{1};
};

}  // namespace pitlane
