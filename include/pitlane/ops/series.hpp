#pragma once

// Market series for the web console: a fixed ring of ticks per instrument, an
// on-demand OHLC aggregation, and a bounded reader for taped history. The rings
// are written by the trading thread and read by the HTTP thread, so every
// accessor is a plain snapshot copy under a seqlock-free contract: the writer
// only advances a monotonic counter after the sample is stored.

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

struct SeriesTick {
    int64_t ts_us{0};
    int64_t bid{0};
    int64_t ask{0};
    int64_t last{0};
    int64_t bid_qty{0};
    int64_t ask_qty{0};
};

struct SeriesBar {
    int64_t ts_us{0};
    int64_t open{0};
    int64_t high{0};
    int64_t low{0};
    int64_t close{0};
    int64_t ticks{0};
};

template <int Cap = 2048>
class SeriesRing {
public:
    static constexpr int kCap = Cap;

    void push(const SeriesTick& t) noexcept {
        buf_[head_] = t;
        head_ = (head_ + 1) % Cap;
        if (n_ < Cap) ++n_;
        ++seq_;
    }

    int size() const noexcept { return n_; }

    // Index 0 is the oldest retained tick.
    SeriesTick at(int i) const noexcept {
        const int start = (head_ + Cap - n_) % Cap;
        return buf_[(start + i) % Cap];
    }

    // Aggregates into `tf_us` buckets and writes up to `cap` bars, oldest
    // first. Returns the number of bars written.
    int bars(int64_t tf_us, int cap, SeriesBar* out) const noexcept {
        if (!out || cap <= 0 || tf_us <= 0 || n_ == 0) return 0;
        int written = 0;
        int64_t bucket = 0;
        for (int i = 0; i < n_; ++i) {
            const SeriesTick t = at(i);
            const int64_t b = t.ts_us / tf_us * tf_us;
            if (written == 0 || b != bucket) {
                if (written == cap) {
                    std::memmove(out, out + 1, static_cast<std::size_t>(cap - 1) * sizeof(SeriesBar));
                    --written;
                }
                SeriesBar bar{};
                bar.ts_us = b;
                bar.open = t.last;
                bar.high = t.last;
                bar.low = t.last;
                bar.close = t.last;
                bar.ticks = 1;
                out[written++] = bar;
                bucket = b;
            } else {
                SeriesBar& bar = out[written - 1];
                if (t.last > bar.high) bar.high = t.last;
                if (t.last < bar.low) bar.low = t.last;
                bar.close = t.last;
                ++bar.ticks;
            }
        }
        return written;
    }

private:
    SeriesTick buf_[Cap]{};
    int head_{0};
    int n_{0};
    uint64_t seq_{0};
};

class SeriesSet {
public:
    static constexpr int kMaxSeries = 8;

    SeriesRing<2048>* find(uint32_t name) noexcept {
        for (int i = 0; i < n_; ++i) {
            if (names_[i] == name) return &rings_[i];
        }
        return nullptr;
    }

    SeriesRing<2048>* ensure(uint32_t name) noexcept {
        if (SeriesRing<2048>* r = find(name)) return r;
        if (n_ >= kMaxSeries) return nullptr;
        names_[n_] = name;
        return &rings_[n_++];
    }

    int count() const noexcept { return n_; }
    uint32_t name_at(int i) const noexcept { return names_[i]; }
    const SeriesRing<2048>& ring_at(int i) const noexcept { return rings_[i]; }

private:
    uint32_t names_[kMaxSeries]{};
    SeriesRing<2048> rings_[kMaxSeries]{};
    int n_{0};
};

// Reads at most `cap` ticks for `name` in [since_us, until_us). The caller
// passes the tape path; malformed rows are skipped like the research loader.
inline int series_read_tape(const char* path, uint32_t name, int64_t since_us, int64_t until_us, int cap,
                            SeriesTick* out) noexcept {
    if (!path || !name || !out || cap <= 0) return 0;
    std::FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (std::fgets(line, sizeof line, f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#' || *p == '\n' || *p == '\r') continue;
        long long t = 0, nm = 0, bid = 0, ask = 0, last = 0, bq = 0, aq = 0;
        if (std::sscanf(p, "%lld,%lld,%lld,%lld,%lld,%lld,%lld", &t, &nm, &bid, &ask, &last, &bq, &aq) != 7) continue;
        if (nm != static_cast<long long>(name) || bid <= 0 || ask <= 0 || last <= 0 || ask < bid) continue;
        if (t < since_us || t >= until_us) continue;
        SeriesTick tick{};
        tick.ts_us = t;
        tick.bid = bid;
        tick.ask = ask;
        tick.last = last;
        tick.bid_qty = bq;
        tick.ask_qty = aq;
        if (n == cap) {
            std::memmove(out, out + 1, static_cast<std::size_t>(cap - 1) * sizeof(SeriesTick));
            --n;
        }
        out[n++] = tick;
    }
    std::fclose(f);
    return n;
}

inline int series_seed_tape(SeriesSet& set, const char* path) noexcept {
    if (!path) return 0;
    std::FILE* f = std::fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int fed = 0;
    while (std::fgets(line, sizeof line, f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#' || *p == '\n' || *p == '\r') continue;
        long long t = 0, nm = 0, bid = 0, ask = 0, last = 0, bq = 0, aq = 0;
        if (std::sscanf(p, "%lld,%lld,%lld,%lld,%lld,%lld,%lld", &t, &nm, &bid, &ask, &last, &bq, &aq) != 7) continue;
        if (nm <= 0 || nm > 0xffffffffll || bid <= 0 || ask <= 0 || last <= 0 || ask < bid) continue;
        SeriesRing<2048>* ring = set.ensure(static_cast<uint32_t>(nm));
        if (!ring) continue;
        SeriesTick tick{};
        tick.ts_us = t;
        tick.bid = bid;
        tick.ask = ask;
        tick.last = last;
        tick.bid_qty = bq;
        tick.ask_qty = aq;
        ring->push(tick);
        ++fed;
    }
    std::fclose(f);
    return fed;
}

} // namespace pitlane
