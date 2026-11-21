#include "pitlane/util/wirelog.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <pthread.h>

namespace pitlane {
namespace {

void append_text(char* out, std::size_t cap, std::size_t& o, const char* s) noexcept {
    if (!s) return;
    while (*s && o + 1 < cap) out[o++] = *s++;
}

void format_line(const PackedLine& rec, char* out, std::size_t cap) noexcept {
    std::size_t o = 0;
    const char* lv = "I ";
    if (rec.level == 0) lv = "E ";
    else if (rec.level == 1) lv = "W ";
    else if (rec.level >= 3) lv = "D ";
    append_text(out, cap, o, lv);
    const uint8_t* p = rec.b + 1;
    const uint8_t* end = rec.b + rec.used;
    const uint8_t n = rec.b[0];
    for (uint8_t i = 0; i < n; ++i) {
        if (p >= end) break;
        const uint8_t tag = *p++;
        char num[64];
        switch (tag) {
        case 0: {
            if (p + 4 > end) break;
            int32_t v = 0;
            std::memcpy(&v, p, 4);
            p += 4;
            std::snprintf(num, sizeof num, "%d", static_cast<int>(v));
            append_text(out, cap, o, num);
            break;
        }
        case 1: {
            if (p + 4 > end) break;
            uint32_t v = 0;
            std::memcpy(&v, p, 4);
            p += 4;
            std::snprintf(num, sizeof num, "%u", v);
            append_text(out, cap, o, num);
            break;
        }
        case 2: {
            if (p + 8 > end) break;
            int64_t v = 0;
            std::memcpy(&v, p, 8);
            p += 8;
            std::snprintf(num, sizeof num, "%lld", static_cast<long long>(v));
            append_text(out, cap, o, num);
            break;
        }
        case 3: {
            if (p + 8 > end) break;
            uint64_t v = 0;
            std::memcpy(&v, p, 8);
            p += 8;
            std::snprintf(num, sizeof num, "%llu", static_cast<unsigned long long>(v));
            append_text(out, cap, o, num);
            break;
        }
        case 4: {
            if (p + 8 > end) break;
            double v = 0;
            std::memcpy(&v, p, 8);
            p += 8;
            std::snprintf(num, sizeof num, "%.6f", v);
            append_text(out, cap, o, num);
            break;
        }
        case 5: {
            if (p + sizeof(LogLit) > end) break;
            LogLit s{};
            std::memcpy(&s, p, sizeof s);
            p += sizeof s;
            append_text(out, cap, o, s.p ? s.p : "");
            break;
        }
        case 6: {
            const char* s = reinterpret_cast<const char*>(p);
            const uint8_t* z = p;
            while (z < end && *z) ++z;
            if (z >= end) break;
            append_text(out, cap, o, s);
            p = z + 1;
            break;
        }
        default:
            i = n;
            break;
        }
        append_text(out, cap, o, " ");
    }
    if (o + 1 < cap) out[o++] = '\n';
    if (o < cap) out[o] = 0;
}

} // namespace

WireLog::WireLog(const char* path, int min_level, int mode)
    : min_(min_level), mode_(mode), ring_(new (std::nothrow) SendJoinRing<PackedLine, 4096>()) {
    if (path && path[0]) fp_ = std::fopen(path, "a");
    if (mode_ == kThread && ring_) {
        running_.store(1, std::memory_order_release);
        auto* thr = new (std::nothrow) pthread_t;
        if (thr && pthread_create(thr, nullptr, &WireLog::thread_main, this) == 0) {
            thr_ = thr;
            thr_on_ = 1;
        } else {
            running_.store(0, std::memory_order_release);
            delete thr;
            mode_ = kCaller;
        }
    }
}

WireLog::~WireLog() {
    running_.store(0, std::memory_order_release);
    if (thr_on_ && thr_) {
        pthread_join(*static_cast<pthread_t*>(thr_), nullptr);
        thr_on_ = 0;
    }
    delete static_cast<pthread_t*>(thr_);
    thr_ = nullptr;
    pump();
    if (fp_) {
        std::fflush(static_cast<FILE*>(fp_));
        std::fclose(static_cast<FILE*>(fp_));
        fp_ = nullptr;
    }
    delete ring_;
    ring_ = nullptr;
}

bool WireLog::pump_one() noexcept {
    if (!ring_) return false;
    PackedLine rec;
    if (!ring_->try_pop(rec)) return false;
    char line[1024];
    line[0] = 0;
    format_line(rec, line, sizeof line);
    std::fwrite(line, 1, std::strlen(line), stderr);
    if (fp_) std::fwrite(line, 1, std::strlen(line), static_cast<FILE*>(fp_));
    drained_.fetch_add(1, std::memory_order_release);
    return true;
}

void WireLog::pump() noexcept {
    int n = 0;
    while (pump_one()) {
        if (++n >= 256) break;
    }
    std::fflush(stderr);
    if (fp_) std::fflush(static_cast<FILE*>(fp_));
}

void WireLog::flush() noexcept {
    if (mode_ == kCaller || !thr_on_) {
        while (pump_one()) {
        }
        std::fflush(stderr);
        if (fp_) std::fflush(static_cast<FILE*>(fp_));
        return;
    }
    const uint64_t n = published_.load(std::memory_order_acquire);
    for (int i = 0; i < 2000 && drained_.load(std::memory_order_acquire) < n; ++i) {
        timespec ts{0, 1000000};
        nanosleep(&ts, nullptr);
    }
    std::fflush(stderr);
    if (fp_) std::fflush(static_cast<FILE*>(fp_));
}

void* WireLog::thread_main(void* self) {
    auto* log = static_cast<WireLog*>(self);
    for (;;) {
        if (log->pump_one()) continue;
        if (!log->running_.load(std::memory_order_acquire)) {
            if (!log->pump_one()) break;
            continue;
        }
        timespec ts{0, 200000};
        nanosleep(&ts, nullptr);
    }
    if (log->fp_) std::fflush(static_cast<FILE*>(log->fp_));
    std::fflush(stderr);
    return nullptr;
}

} // namespace pitlane
