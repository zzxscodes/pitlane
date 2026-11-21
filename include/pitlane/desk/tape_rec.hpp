#pragma once

#include "pitlane/desk/types.hpp"
#include "pitlane/util/hint.hpp"
#include "pitlane/util/sendjoin.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace pitlane {

// Records a Quote stream into the replayable seven-column CSV tape.
// One producer (MD callback), one writer thread. A full ring drops the quote
// and counts it; the hot path never blocks and never allocates.
// close() flushes, fsyncs and renames path.tmp onto path, so a reader never
// sees a partially written tape.

class TapeRecorder {
public:
    static constexpr uint32_t kRing = 8192;

    TapeRecorder() = default;
    ~TapeRecorder();
    TapeRecorder(const TapeRecorder&) = delete;
    TapeRecorder& operator=(const TapeRecorder&) = delete;

    bool open(const char* path) noexcept;
    void close() noexcept;

    PITLANE_HOT bool note(const Quote& q) noexcept;
    bool active() const noexcept { return running_.load(std::memory_order_acquire) != 0; }
    const char* path() const noexcept { return final_path_; }
    uint64_t written() const noexcept { return written_.load(std::memory_order_relaxed); }
    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    static void* thread_main(void* self);
    bool drain() noexcept;

    SendJoinRing<Quote, kRing>* ring_{nullptr};
    std::FILE* fp_{nullptr};
    void* thr_{nullptr};
    std::atomic<uint8_t> running_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> dropped_{0};
    char tmp_path_[512]{};
    char final_path_[512]{};
};

} // namespace pitlane
