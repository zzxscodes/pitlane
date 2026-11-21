#include "pitlane/desk/tape_rec.hpp"

#include <cstring>
#include <ctime>
#include <new>
#include <pthread.h>
#include <unistd.h>

namespace pitlane {
namespace {

constexpr const char* kHeader = "# pitlane tape v1 exch_us,name,bid_px,ask_px,last_px,bid_qty,ask_qty\n";
constexpr std::size_t kBatch = 65536;

// One CSV row, no trailing newline. Returns the byte count written (0 on overflow).
int format_row(const Quote& q, char* out, std::size_t cap) noexcept {
    const int n = std::snprintf(out, cap, "%lld,%u,%lld,%lld,%lld,%lld,%lld\n",
                                static_cast<long long>(q.exch_us), q.name, static_cast<long long>(q.bid_px),
                                static_cast<long long>(q.ask_px), static_cast<long long>(q.last_px),
                                static_cast<long long>(q.bid_qty), static_cast<long long>(q.ask_qty));
    if (n < 0 || static_cast<std::size_t>(n) >= cap) return 0;
    return n;
}

} // namespace

TapeRecorder::~TapeRecorder() { close(); }

bool TapeRecorder::open(const char* path) noexcept {
    if (active()) return false;
    if (!path || !path[0]) return false;
    const std::size_t n = std::strlen(path);
    if (n + 5 >= sizeof final_path_) return false;
    if (path[n - 1] == '/') return false;
    std::memcpy(final_path_, path, n + 1);
    std::memcpy(tmp_path_, path, n);
    std::memcpy(tmp_path_ + n, ".tmp", 5);

    ring_ = new (std::nothrow) SendJoinRing<Quote, kRing>();
    if (!ring_) return false;
    fp_ = std::fopen(tmp_path_, "w");
    if (!fp_) {
        delete ring_;
        ring_ = nullptr;
        tmp_path_[0] = 0;
        final_path_[0] = 0;
        return false;
    }
    std::fwrite(kHeader, 1, std::strlen(kHeader), fp_);

    written_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    running_.store(1, std::memory_order_release);
    auto* thr = new (std::nothrow) pthread_t;
    if (!thr || pthread_create(thr, nullptr, &TapeRecorder::thread_main, this) != 0) {
        running_.store(0, std::memory_order_release);
        delete thr;
        std::fclose(fp_);
        fp_ = nullptr;
        std::remove(tmp_path_);
        delete ring_;
        ring_ = nullptr;
        tmp_path_[0] = 0;
        final_path_[0] = 0;
        return false;
    }
    thr_ = thr;
    return true;
}

void TapeRecorder::close() noexcept {
    running_.store(0, std::memory_order_release);
    if (thr_) {
        pthread_join(*static_cast<pthread_t*>(thr_), nullptr);
        delete static_cast<pthread_t*>(thr_);
        thr_ = nullptr;
    }
    if (ring_) (void)drain();
    bool renamed = false;
    if (fp_) {
        std::fflush(fp_);
        (void)::fsync(::fileno(fp_));
        std::fclose(fp_);
        fp_ = nullptr;
        if (tmp_path_[0] && final_path_[0]) renamed = std::rename(tmp_path_, final_path_) == 0;
    }
    if (!renamed && tmp_path_[0]) std::remove(tmp_path_);
    delete ring_;
    ring_ = nullptr;
    tmp_path_[0] = 0;
    final_path_[0] = 0;
}

PITLANE_HOT bool TapeRecorder::note(const Quote& q) noexcept {
    if (PITLANE_UNLIKELY(!ring_ || !running_.load(std::memory_order_relaxed))) return false;
    if (PITLANE_UNLIKELY(!ring_->try_push(q))) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool TapeRecorder::drain() noexcept {
    if (!ring_ || !fp_) return false;
    char buf[kBatch];
    std::size_t used = 0;
    bool any = false;
    Quote q{};
    while (ring_->try_pop(q)) {
        if (used + 160 > sizeof buf) {
            std::fwrite(buf, 1, used, fp_);
            used = 0;
        }
        const int n = format_row(q, buf + used, sizeof buf - used);
        if (n <= 0) continue;
        used += static_cast<std::size_t>(n);
        written_.fetch_add(1, std::memory_order_relaxed);
        any = true;
    }
    if (used) std::fwrite(buf, 1, used, fp_);
    return any;
}

void* TapeRecorder::thread_main(void* self) {
    auto* rec = static_cast<TapeRecorder*>(self);
    char buf[kBatch];
    std::size_t used = 0;
    for (;;) {
        Quote q{};
        bool got = false;
        while (rec->ring_ && rec->ring_->try_pop(q)) {
            if (used + 160 > sizeof buf) {
                std::fwrite(buf, 1, used, rec->fp_);
                used = 0;
            }
            const int n = format_row(q, buf + used, sizeof buf - used);
            if (n <= 0) continue;
            used += static_cast<std::size_t>(n);
            rec->written_.fetch_add(1, std::memory_order_relaxed);
            got = true;
        }
        if (used) {
            std::fwrite(buf, 1, used, rec->fp_);
            used = 0;
        }
        if (got) continue;
        if (!rec->running_.load(std::memory_order_acquire)) break;
        timespec ts{0, 200000};
        nanosleep(&ts, nullptr);
    }
    std::fflush(rec->fp_);
    return nullptr;
}

} // namespace pitlane
