#pragma once

// Append-only, columnar tick tape. Cold path only: the trading thread never
// touches this file. Layout is a 64-byte file header, then a sequence of fixed
// 64 KiB blocks (8 columns x 1024 rows of int64), then 64-byte commit records.
// Each commit publishes a version; readers can query an older version, project
// a column subset, and skip blocks by time without scanning the tape.
//
// Design:
// - No library dependency, no exceptions, no RTTI.
// - Blocks are immutable once written, so a committed version never changes.
// - Reads use pread into cache-line-aligned staging buffers; the file is never
//   mapped, so a truncated file cannot raise SIGBUS.

#include "pitlane/desk/types.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pitlane {

inline constexpr uint32_t kTapeColumns = 8;
inline constexpr uint32_t kTapeBlockRows = 1024;
inline constexpr uint32_t kTapeBlockBytes = kTapeBlockRows * kTapeColumns * 8; // 64 KiB
inline constexpr uint32_t kTapeHeaderBytes = 64;
inline constexpr uint32_t kTapeCommitBytes = 64;
// 夜盘跨 00:00 时 CTP 的 exch_us 会回退一天，写入时按连续会话时钟补一天。
inline constexpr int64_t kTapeDayUs = 86400ll * 1000000;
inline constexpr int64_t kTapeWrapUs = 12ll * 3600 * 1000000;

// Column bits for rows() projection. The order matches the on-disk order.
enum : uint8_t {
    kTapeColExch = 1u << 0,
    kTapeColLocal = 1u << 1,
    kTapeColBid = 1u << 2,
    kTapeColAsk = 1u << 3,
    kTapeColLast = 1u << 4,
    kTapeColBidQty = 1u << 5,
    kTapeColAskQty = 1u << 6,
    kTapeColLastQty = 1u << 7,
    kTapeColAll = 0xff,
};

struct TapeTick {
    int64_t exch_us{0};
    int64_t local_us{0};
    int64_t bid_px{0};
    int64_t ask_px{0};
    int64_t last_px{0};
    int64_t bid_qty{0};
    int64_t ask_qty{0};
    int64_t last_qty{0};
};

struct TapeColStats {
    int64_t min{0};
    int64_t max{0};
    int64_t last{0};
    double mean{0};
    uint64_t rows{0};
};

struct TapeStats {
    uint32_t version{0};
    uint64_t rows{0};
    TapeColStats col[kTapeColumns]{};
};

struct TapeMeta {
    uint32_t versions{0};
    uint32_t blocks{0};
    uint32_t symbol{0};
    uint64_t rows{0};
    uint64_t bytes{0};
    int64_t first_ts{0};
    int64_t last_ts{0};
};

namespace tape_detail {

inline constexpr char kFileMagic[8] = {'P', 'L', 'T', 'A', 'P', 'E', '0', '1'};
inline constexpr char kBlockMagic[8] = {'P', 'L', 'B', 'L', 'K', '0', '0', '1'};
inline constexpr char kCommitMagic[8] = {'P', 'L', 'C', 'M', 'T', '0', '0', '1'};

inline bool same_magic(const char* a, const char* b) noexcept { return std::memcmp(a, b, 8) == 0; }

inline void put8(char* d, const char* s) noexcept { std::memcpy(d, s, 8); }

template <class T>
inline void put(char* d, std::size_t off, T v) noexcept {
    std::memcpy(d + off, &v, sizeof v);
}

template <class T>
inline T get(const char* s, std::size_t off) noexcept {
    T v{};
    std::memcpy(&v, s + off, sizeof v);
    return v;
}

inline bool read_full(int fd, void* buf, std::size_t n, off_t off) noexcept {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        const ssize_t got = ::pread(fd, p, n, off);
        if (got <= 0) return false;
        p += got;
        off += got;
        n -= static_cast<std::size_t>(got);
    }
    return true;
}

inline bool write_full(int fd, const void* buf, std::size_t n) noexcept {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        const ssize_t put_n = ::write(fd, p, n);
        if (put_n <= 0) return false;
        p += put_n;
        n -= static_cast<std::size_t>(put_n);
    }
    return true;
}

// True when the file starts with the block-tape magic.
inline bool is_block_tape(const char* path) noexcept {
    if (!path || !path[0]) return false;
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    char magic[8];
    const ssize_t got = ::pread(fd, magic, sizeof magic, 0);
    ::close(fd);
    return got == static_cast<ssize_t>(sizeof magic) && same_magic(magic, kFileMagic);
}

} // namespace tape_detail

// Writes one symbol to one file. append() batches rows, commit() publishes a
// new immutable version and is the only place that fsyncs.
class BlockTapeWriter {
public:
    BlockTapeWriter() = default;
    BlockTapeWriter(const BlockTapeWriter&) = delete;
    BlockTapeWriter& operator=(const BlockTapeWriter&) = delete;
    ~BlockTapeWriter() { close(); }

    // Creates or truncates the file.
    bool open(const char* path, uint32_t symbol = 0) noexcept {
        close();
        if (!path || !path[0]) return false;
        fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) return false;
        char h[kTapeHeaderBytes];
        std::memset(h, 0, sizeof h);
        tape_detail::put8(h, tape_detail::kFileMagic);
        tape_detail::put<uint32_t>(h, 8, 1); // format version
        tape_detail::put<uint32_t>(h, 12, kTapeColumns);
        tape_detail::put<uint32_t>(h, 16, kTapeBlockRows);
        tape_detail::put<uint32_t>(h, 20, symbol);
        if (!tape_detail::write_full(fd_, h, sizeof h)) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        off_ = kTapeHeaderBytes;
        symbol_ = symbol;
        last_raw_ts_ = 0;
        day_off_us_ = 0;
        return true;
    }

    bool append(const TapeTick* t, int n) noexcept {
        if (fd_ < 0 || !t || n < 0) return false;
        int i = 0;
        while (i < n) {
            const uint32_t room = kTapeBlockRows - pending_;
            uint32_t take = static_cast<uint32_t>(n - i);
            if (take > room) take = room;
            for (uint32_t k = 0; k < take; ++k) {
                // 跨午夜的时间回退按新的一天累加，块内和整个文件都保持单调。
                const int64_t raw = t[i + k].exch_us;
                int64_t ts = raw;
                if (raw > 0) {
                    if (last_raw_ts_ > 0 && raw < last_raw_ts_ && last_raw_ts_ - raw >= kTapeWrapUs) {
                        day_off_us_ += kTapeDayUs;
                    }
                    last_raw_ts_ = raw;
                    ts = raw + day_off_us_;
                }
                stage_[0][pending_ + k] = ts;
                stage_[1][pending_ + k] = t[i + k].local_us;
                stage_[2][pending_ + k] = t[i + k].bid_px;
                stage_[3][pending_ + k] = t[i + k].ask_px;
                stage_[4][pending_ + k] = t[i + k].last_px;
                stage_[5][pending_ + k] = t[i + k].bid_qty;
                stage_[6][pending_ + k] = t[i + k].ask_qty;
                stage_[7][pending_ + k] = t[i + k].last_qty;
            }
            pending_ += take;
            uncommitted_ += take;
            i += static_cast<int>(take);
            if (pending_ == kTapeBlockRows && !flush_block()) return false;
        }
        return true;
    }

    bool commit() noexcept {
        if (fd_ < 0 || uncommitted_ == 0) return false;
        if (pending_ > 0 && !flush_block()) return false;
        if (versions_ == 0xffffffffu) return false;
        char c[kTapeCommitBytes];
        std::memset(c, 0, sizeof c);
        tape_detail::put8(c, tape_detail::kCommitMagic);
        tape_detail::put<uint32_t>(c, 8, versions_ + 1);
        tape_detail::put<uint32_t>(c, 12, static_cast<uint32_t>(rows_));
        tape_detail::put<uint64_t>(c, 16, off_);
        tape_detail::put<int64_t>(c, 24, first_ts_);
        tape_detail::put<int64_t>(c, 32, last_ts_);
        tape_detail::put<uint32_t>(c, 40, blocks_);
        if (!tape_detail::write_full(fd_, c, sizeof c)) return false;
        off_ += sizeof c;
        ++versions_;
        uncommitted_ = 0;
        return ::fsync(fd_) == 0;
    }

    bool close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        return true;
    }

    uint64_t rows() const noexcept { return rows_; }
    uint32_t versions() const noexcept { return versions_; }

private:
    bool flush_block() noexcept {
        if (pending_ == 0) return true;
        char h[kTapeHeaderBytes];
        std::memset(h, 0, sizeof h);
        tape_detail::put8(h, tape_detail::kBlockMagic);
        tape_detail::put<uint32_t>(h, 8, pending_);
        tape_detail::put<uint32_t>(h, 12, blocks_);
        tape_detail::put<int64_t>(h, 16, stage_[0][0]);
        tape_detail::put<int64_t>(h, 24, stage_[0][pending_ - 1]);
        // Pad the tail so every block occupies a fixed 64 KiB payload.
        for (uint32_t r = pending_; r < kTapeBlockRows; ++r) {
            for (uint32_t c = 0; c < kTapeColumns; ++c) stage_[c][r] = 0;
        }
        if (!tape_detail::write_full(fd_, h, sizeof h)) return false;
        if (!tape_detail::write_full(fd_, stage_, sizeof stage_)) return false;
        off_ += kTapeHeaderBytes + kTapeBlockBytes;
        const int64_t last_ts = stage_[0][pending_ - 1];
        if (blocks_ == 0) first_ts_ = stage_[0][0];
        last_ts_ = last_ts;
        rows_ += pending_;
        ++blocks_;
        pending_ = 0;
        return true;
    }

    int fd_{-1};
    uint32_t pending_{0};
    uint32_t uncommitted_{0};
    uint32_t blocks_{0};
    uint32_t versions_{0};
    uint32_t symbol_{0};
    uint64_t rows_{0};
    uint64_t off_{0};
    int64_t first_ts_{0};
    int64_t last_ts_{0};
    int64_t last_raw_ts_{0}; // 归一化前的上一行 exch_us
    int64_t day_off_us_{0};  // 跨午夜累计的天数偏移
    alignas(64) int64_t stage_[kTapeColumns][kTapeBlockRows]{};
};

// Read side. open() validates the file and caches the block index; a query then
// touches only the blocks and columns it needs.
class BlockTapeReader {
public:
    BlockTapeReader() = default;
    BlockTapeReader(const BlockTapeReader&) = delete;
    BlockTapeReader& operator=(const BlockTapeReader&) = delete;
    ~BlockTapeReader() { close(); }

    bool open(const char* path) noexcept {
        close();
        if (!path || !path[0]) return false;
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) return false;
        struct stat st {};
        if (::fstat(fd_, &st) != 0 || st.st_size < static_cast<off_t>(kTapeHeaderBytes)) {
            close();
            return false;
        }
        size_ = static_cast<uint64_t>(st.st_size);
        char h[kTapeHeaderBytes];
        if (!tape_detail::read_full(fd_, h, sizeof h, 0) || !tape_detail::same_magic(h, tape_detail::kFileMagic)) {
            close();
            return false;
        }
        if (tape_detail::get<uint32_t>(h, 8) != 1 || tape_detail::get<uint32_t>(h, 12) != kTapeColumns ||
            tape_detail::get<uint32_t>(h, 16) != kTapeBlockRows) {
            close();
            return false;
        }
        symbol_ = tape_detail::get<uint32_t>(h, 20);
        if (!scan()) {
            close();
            return false;
        }
        return true;
    }

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        size_ = 0;
        bytes_ = 0;
        rows_ = 0;
        symbol_ = 0;
        first_ts_ = 0;
        last_ts_ = 0;
        touched_ = 0;
        blocks_.clear();
        commits_.clear();
    }

    bool meta(TapeMeta& m) const noexcept {
        if (fd_ < 0 || commits_.empty()) return false;
        m.versions = static_cast<uint32_t>(commits_.size());
        m.blocks = static_cast<uint32_t>(blocks_.size());
        m.symbol = symbol_;
        m.rows = rows_;
        m.bytes = bytes_;
        m.first_ts = first_ts_;
        m.last_ts = last_ts_;
        return true;
    }

    uint64_t rows_of(uint32_t version) const noexcept {
        if (version == 0) version = static_cast<uint32_t>(commits_.size());
        if (version < 1 || version > static_cast<uint32_t>(commits_.size())) return 0;
        return commits_[version - 1].rows;
    }

    // Copies rows of `version` (0 = latest). Time bounds are inclusive; pass
    // INT64_MIN / INT64_MAX for none. offset/limit apply after the time filter.
    bool rows(uint32_t version, int64_t since_us, int64_t until_us, uint32_t offset, uint32_t limit, uint8_t cols,
              TapeTick* out, uint32_t cap, uint32_t& n) noexcept {
        n = 0;
        if (fd_ < 0 || !out || cap == 0) return false;
        if (version == 0) version = static_cast<uint32_t>(commits_.size());
        if (version < 1 || version > static_cast<uint32_t>(commits_.size())) return false;
        const uint64_t avail = commits_[version - 1].rows;
        if (cols == 0) cols = kTapeColAll;
        const uint32_t max_out = limit == 0 || limit > cap ? cap : limit;

        uint32_t first = 0;
        uint32_t last = static_cast<uint32_t>(blocks_.size());
        if (since_us != INT64_MIN) {
            while (first < last && blocks_[first].last_ts < since_us) ++first;
        }
        if (until_us != INT64_MAX) {
            while (last > first && blocks_[last - 1].first_ts > until_us) --last;
        }

        uint64_t skipped = 0;
        for (uint32_t b = first; b < last && n < max_out; ++b) {
            const BlockRef& r = blocks_[b];
            if (r.first_row >= avail) break;
            const uint64_t room = avail - r.first_row;
            const uint32_t usable = static_cast<uint32_t>(room < r.rows ? room : r.rows);
            uint32_t lo = 0;
            uint32_t hi = usable;
            if (since_us != INT64_MIN) lo = lower_bound(r, usable, since_us);
            if (until_us != INT64_MAX) hi = upper_bound(r, usable, until_us);
            if (hi <= lo) continue;
            const uint32_t count = hi - lo;
            if (skipped + count <= offset) {
                skipped += count;
                continue;
            }
            uint32_t start = lo;
            if (skipped < offset) start += static_cast<uint32_t>(offset - skipped);
            uint32_t want = hi - start;
            if (want > max_out - n) want = max_out - n;
            if (!read_columns(r, start, want, cols, out + n)) return false;
            n += want;
            skipped = offset;
        }
        return true;
    }

    // Convenience for research and backtest: whole version into a Quote array.
    bool load_quotes(uint32_t version, Quote* out, uint32_t cap, uint32_t& n) noexcept {
        n = 0;
        if (fd_ < 0 || !out || cap == 0) return false;
        if (version == 0) version = static_cast<uint32_t>(commits_.size());
        if (version < 1 || version > static_cast<uint32_t>(commits_.size())) return false;
        const uint64_t avail = commits_[version - 1].rows;
        for (const BlockRef& r : blocks_) {
            if (n >= cap) break;
            if (r.first_row >= avail) break;
            const uint64_t room = avail - r.first_row;
            uint32_t usable = static_cast<uint32_t>(room < r.rows ? room : r.rows);
            if (usable > cap - n) usable = cap - n;
            if (!read_columns(r, 0, usable, kTapeColAll, nullptr, out + n)) return false;
            n += usable;
        }
        return true;
    }

    bool describe(uint32_t version, TapeStats& st) noexcept {
        st = TapeStats{};
        if (fd_ < 0) return false;
        if (version == 0) version = static_cast<uint32_t>(commits_.size());
        if (version < 1 || version > static_cast<uint32_t>(commits_.size())) return false;
        const uint64_t avail = commits_[version - 1].rows;
        st.version = version;
        st.rows = avail;
        double sum[kTapeColumns]{};
        bool first = true;
        for (const BlockRef& r : blocks_) {
            if (r.first_row >= avail) break;
            const uint64_t room = avail - r.first_row;
            const uint32_t usable = static_cast<uint32_t>(room < r.rows ? room : r.rows);
            if (!read_columns(r, 0, usable, kTapeColAll, nullptr, nullptr)) return false;
            for (uint32_t c = 0; c < kTapeColumns; ++c) {
                TapeColStats& s = st.col[c];
                s.rows = avail;
                for (uint32_t i = 0; i < usable; ++i) {
                    const int64_t v = col_[c][i];
                    if (first && i == 0) {
                        s.min = v;
                        s.max = v;
                    } else {
                        if (v < s.min) s.min = v;
                        if (v > s.max) s.max = v;
                    }
                    s.last = v;
                    sum[c] += static_cast<double>(v);
                }
            }
            first = false;
        }
        if (avail > 0) {
            for (uint32_t c = 0; c < kTapeColumns; ++c) st.col[c].mean = sum[c] / static_cast<double>(avail);
        }
        return true;
    }

    uint32_t blocks_read() const noexcept { return touched_; }
    void reset_blocks_read() noexcept { touched_ = 0; }
    uint32_t symbol() const noexcept { return symbol_; }

private:
    struct BlockRef {
        uint64_t off{0}; // payload offset
        uint64_t first_row{0};
        uint32_t rows{0};
        uint32_t seq{0};
        int64_t first_ts{0};
        int64_t last_ts{0};
    };
    struct CommitRef {
        uint32_t version{0};
        uint64_t rows{0};
        uint64_t end_off{0};
        int64_t first_ts{0};
        int64_t last_ts{0};
    };

    bool read_ts(const BlockRef& r, uint32_t i, int64_t& v) noexcept {
        const off_t off = static_cast<off_t>(r.off + static_cast<uint64_t>(i) * 8);
        return tape_detail::read_full(fd_, &v, sizeof v, off);
    }

    uint32_t lower_bound(const BlockRef& r, uint32_t usable, int64_t since) noexcept {
        uint32_t lo = 0;
        uint32_t hi = usable;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            int64_t v = 0;
            if (!read_ts(r, mid, v) || v < since) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    uint32_t upper_bound(const BlockRef& r, uint32_t usable, int64_t until) noexcept {
        uint32_t lo = 0;
        uint32_t hi = usable;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            int64_t v = 0;
            if (!read_ts(r, mid, v) || v <= until) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    bool scan() noexcept {
        uint64_t pos = kTapeHeaderBytes;
        uint64_t cum = 0;
        while (pos + kTapeHeaderBytes <= size_) {
            char tag[kTapeHeaderBytes];
            if (!tape_detail::read_full(fd_, tag, sizeof tag, static_cast<off_t>(pos))) return false;
            if (tape_detail::same_magic(tag, tape_detail::kBlockMagic)) {
                if (pos + kTapeHeaderBytes + kTapeBlockBytes > size_) return false;
                BlockRef r{};
                r.rows = tape_detail::get<uint32_t>(tag, 8);
                r.seq = tape_detail::get<uint32_t>(tag, 12);
                r.first_ts = tape_detail::get<int64_t>(tag, 16);
                r.last_ts = tape_detail::get<int64_t>(tag, 24);
                if (r.rows == 0 || r.rows > kTapeBlockRows) return false;
                r.off = pos + kTapeHeaderBytes;
                r.first_row = cum;
                blocks_.push_back(r);
                cum += r.rows;
                pos += kTapeHeaderBytes + kTapeBlockBytes;
            } else if (tape_detail::same_magic(tag, tape_detail::kCommitMagic)) {
                CommitRef c{};
                c.version = tape_detail::get<uint32_t>(tag, 8);
                c.rows = tape_detail::get<uint32_t>(tag, 12);
                c.end_off = tape_detail::get<uint64_t>(tag, 16);
                c.first_ts = tape_detail::get<int64_t>(tag, 24);
                c.last_ts = tape_detail::get<int64_t>(tag, 32);
                if (c.version != static_cast<uint32_t>(commits_.size()) + 1 || c.rows != cum || c.rows == 0) return false;
                commits_.push_back(c);
                pos += kTapeCommitBytes;
            } else {
                return false;
            }
        }
        if (pos != size_) return false; // truncated or trailing garbage
        if (commits_.empty()) return false;
        bytes_ = size_;
        rows_ = commits_.back().rows;
        first_ts_ = commits_.front().first_ts;
        last_ts_ = commits_.back().last_ts;
        return true;
    }

    // Reads `count` rows from `start` into col_[]; copies into ticks or quotes.
    // Exactly one of ticks/quotes must be non-null.
    bool read_columns(const BlockRef& r, uint32_t start, uint32_t count, uint8_t cols, TapeTick* ticks,
                      Quote* quotes = nullptr) noexcept {
        if (count == 0) return true;
        if (start + count > r.rows) return false;
        for (uint32_t c = 0; c < kTapeColumns; ++c) {
            if (!(cols & (1u << c))) continue;
            const off_t off = static_cast<off_t>(r.off + static_cast<uint64_t>(c) * kTapeBlockRows * 8 +
                                                 static_cast<uint64_t>(start) * 8);
            if (!tape_detail::read_full(fd_, col_[c], sizeof(int64_t) * count, off)) return false;
        }
        ++touched_;
        if (!ticks && !quotes) return true; // describe() only needs col_
        for (uint32_t i = 0; i < count; ++i) {
            if (ticks) {
                TapeTick& t = ticks[i];
                if (cols & kTapeColExch) t.exch_us = col_[0][i];
                if (cols & kTapeColLocal) t.local_us = col_[1][i];
                if (cols & kTapeColBid) t.bid_px = col_[2][i];
                if (cols & kTapeColAsk) t.ask_px = col_[3][i];
                if (cols & kTapeColLast) t.last_px = col_[4][i];
                if (cols & kTapeColBidQty) t.bid_qty = col_[5][i];
                if (cols & kTapeColAskQty) t.ask_qty = col_[6][i];
                if (cols & kTapeColLastQty) t.last_qty = col_[7][i];
            } else {
                Quote& q = quotes[i];
                q = Quote{};
                q.name = symbol_;
                q.exch_us = col_[0][i];
                q.local_us = col_[1][i];
                q.bid_px = col_[2][i];
                q.ask_px = col_[3][i];
                q.last_px = col_[4][i];
                q.bid_qty = col_[5][i];
                q.ask_qty = col_[6][i];
                q.last_qty = col_[7][i];
            }
        }
        return true;
    }

    int fd_{-1};
    uint64_t size_{0};
    uint64_t bytes_{0};
    uint64_t rows_{0};
    uint32_t symbol_{0};
    int64_t first_ts_{0};
    int64_t last_ts_{0};
    uint32_t touched_{0};
    std::vector<BlockRef> blocks_;
    std::vector<CommitRef> commits_;
    alignas(64) int64_t col_[kTapeColumns][kTapeBlockRows]{};
};

} // namespace pitlane
