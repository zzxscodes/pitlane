// Cold-path job worker for the web console: research grid sweeps and historical
// backtests run on one background thread, never on the trading thread. Requests
// arrive through a fixed SPSC ring; results are published through seqlocks.

#pragma once

#include "pitlane/study/backtest.hpp"
#include "pitlane/study/factor_lab.hpp"
#include "pitlane/study/tape_source.hpp"
#include "pitlane/util/sendjoin.hpp"
#include "research.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <time.h>
#include <vector>

namespace trading_jobs {

enum : uint8_t { kJobIdle = 0, kJobQueued = 1, kJobRunning = 2, kJobDone = 3, kJobFailed = 4 };

inline const char* job_state_name(uint8_t s) noexcept {
    switch (s) {
        case kJobQueued: return "queued";
        case kJobRunning: return "running";
        case kJobDone: return "done";
        case kJobFailed: return "failed";
        default: return "idle";
    }
}

struct JobReq {
    uint64_t id{0};
    uint8_t kind{0};  // 1 research, 2 backtest
    uint32_t name{0};
    char tape[256]{};
    uint8_t factor_kind{2};
    uint16_t window{16};
    int8_t sign{1};
    int64_t threshold_ppm{10};
    int64_t qty{1};
    int64_t fee_ppm{20};
    int64_t cash{1000000};
};

struct ResearchRowOut {
    int32_t kind{0};
    int32_t window{0};
    int32_t horizon{0};
    int64_t n{0};
    int64_t ic{0};
    int64_t rank_ic{0};
    int64_t hit{0};
    int64_t turnover{0};
    int64_t icir{0};
    int64_t score{0};
};

struct ResearchResult {
    uint64_t id{0};
    uint8_t state{0};
    int64_t started_us{0};
    int64_t done_us{0};
    int32_t rows_n{0};
    ResearchRowOut rows[pitlane::FactorLab::kMaxRows]{};
    uint8_t best_kind{0};
    uint16_t best_window{0};
    int32_t best_horizon{0};
    int8_t best_sign{1};
    int64_t best_threshold_ppm{0};
    int64_t best_n{0};
    int64_t best_ic{0};
    int64_t best_rank_ic{0};
    int64_t best_hit{0};
    int64_t best_turnover{0};
    int64_t best_score{0};
    char note[96]{};
};

struct CurvePointOut {
    int64_t ts_us{0};
    int64_t equity{0};
    int64_t dd_ppm{0};
};

struct BacktestResult {
    uint64_t id{0};
    uint8_t state{0};
    int64_t started_us{0};
    int64_t done_us{0};
    uint8_t kind{0};
    uint16_t window{0};
    int8_t sign{1};
    int64_t threshold_ppm{0};
    int64_t qty{0};
    int64_t fee_ppm{0};
    int64_t cash{0};
    int64_t points{0};
    int64_t trades{0};
    int64_t round_trips{0};
    int64_t wins{0};
    int64_t gross_pnl{0};
    int64_t fees{0};
    int64_t net_pnl{0};
    int64_t turnover{0};
    int64_t return_ppm{0};
    int64_t annual_ppm{0};
    int64_t sharpe_ppm{0};
    int64_t max_dd_ppm{0};
    int64_t win_ppm{0};
    int64_t exposure_ppm{0};
    int64_t final_equity{0};
    int64_t span_us{0};
    int32_t curve_n{0};
    CurvePointOut curve[512]{};
    char note[96]{};
};

template <class T>
struct SeqCell {
    alignas(64) std::atomic<uint64_t> seq{0};
    T val{};

    void publish(const T& v) noexcept {
        const uint64_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        val = v;
        seq.store(s + 2, std::memory_order_release);
    }

    bool read(T& out) const noexcept {
        for (int spin = 0; spin < 64; ++spin) {
            const uint64_t s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1ull) continue;
            const T tmp = val;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_acquire) == s1) {
                out = tmp;
                return true;
            }
        }
        return false;
    }
};

class JobWorker {
public:
    static constexpr uint32_t kReqCap = 8;

    ~JobWorker() { stop(); }

    bool start() noexcept {
        if (started_) return true;
        if (!run_.exchange(true, std::memory_order_acq_rel)) {
            if (pthread_create(&tid_, nullptr, &JobWorker::thread_main, this) != 0) {
                run_.store(false, std::memory_order_release);
                return false;
            }
        }
        started_ = true;
        return true;
    }

    void stop() noexcept {
        if (!started_) return;
        run_.store(false, std::memory_order_release);
        pthread_join(tid_, nullptr);
        started_ = false;
    }

    // Fills `r.id` when the caller left it zero.
    bool submit(JobReq& r) noexcept {
        if (!started_) return false;
        if (r.id == 0) r.id = next_id_.fetch_add(1, std::memory_order_relaxed);
        if (!q_.try_push(r)) return false;
        if (r.kind == 2) backtest_req_.store(r.id, std::memory_order_release);
        else research_req_.store(r.id, std::memory_order_release);
        return true;
    }

    bool research(ResearchResult& out) const noexcept { return research_.read(out); }
    bool backtest(BacktestResult& out) const noexcept { return backtest_.read(out); }

    // The newest id accepted by submit(). When it differs from the id returned by
    // research()/backtest(), that job has not published a state yet, so readers
    // must report it as queued instead of showing the previous run's result.
    uint64_t research_req_id() const noexcept { return research_req_.load(std::memory_order_acquire); }
    uint64_t backtest_req_id() const noexcept { return backtest_req_.load(std::memory_order_acquire); }

private:
    static void* thread_main(void* user) {
        static_cast<JobWorker*>(user)->loop();
        return nullptr;
    }

    void loop() noexcept {
        while (run_.load(std::memory_order_acquire)) {
            JobReq r{};
            if (q_.try_pop(r)) {
                run_job(r);
                continue;
            }
            timespec ts{0, 1000000};
            nanosleep(&ts, nullptr);
        }
    }

    void run_job(const JobReq& r) noexcept {
        if (r.kind == 2) {
            run_backtest(r);
        } else {
            run_research(r);
        }
    }

    void run_research(const JobReq& r) noexcept {
        ResearchResult out{};
        out.id = r.id;
        out.state = kJobRunning;
        out.started_us = now_us();
        research_.publish(out);

        std::vector<pitlane::Quote> tape;
        char source[40]{};
        load_tape(r.tape, r.name, tape, source, sizeof source);
        const int n = static_cast<int>(tape.size());
        if (n < 64) {
            out.state = kJobFailed;
            out.done_us = now_us();
            std::snprintf(out.note, sizeof out.note, "tape too short (%d rows)", n);
            research_.publish(out);
            return;
        }

        int stride = n / 4096;
        if (stride < 1) stride = 1;
        const int split = n * 7 / 10;
        pitlane::FactorLab lab;
        pitlane::LabRow rows[pitlane::FactorLab::kMaxRows];
        const int nr = lab.sweep(tape.data(), n, stride, 0, split, rows, pitlane::FactorLab::kMaxRows);
        if (nr <= 0) {
            out.state = kJobFailed;
            out.done_us = now_us();
            std::snprintf(out.note, sizeof out.note, "no combo reached the sample floor");
            research_.publish(out);
            return;
        }

        out.rows_n = nr;
        for (int i = 0; i < nr; ++i) {
            ResearchRowOut& d = out.rows[i];
            d.kind = rows[i].kind;
            d.window = rows[i].window;
            d.horizon = rows[i].horizon;
            d.n = rows[i].n;
            d.ic = rows[i].ic_ppm;
            d.rank_ic = rows[i].rank_ic_ppm;
            d.hit = rows[i].hit_ppm;
            d.turnover = rows[i].turnover_ppm;
            d.icir = rows[i].icir_milli;
            d.score = rows[i].score_ppm;
        }
        pitlane::LabPick pick{};
        if (lab.pick(tape.data(), n, rows[0], stride, split, pick)) {
            out.best_kind = rows[0].kind;
            out.best_window = rows[0].window;
            out.best_horizon = rows[0].horizon;
            out.best_sign = pick.sign;
            out.best_threshold_ppm = pick.threshold_ppm;
        } else {
            out.best_kind = rows[0].kind;
            out.best_window = rows[0].window;
            out.best_horizon = rows[0].horizon;
            out.best_sign = rows[0].rank_ic_ppm < 0 ? -1 : 1;
            out.best_threshold_ppm = 1;
        }
        out.best_n = rows[0].n;
        out.best_ic = rows[0].ic_ppm;
        out.best_rank_ic = rows[0].rank_ic_ppm;
        out.best_hit = rows[0].hit_ppm;
        out.best_turnover = rows[0].turnover_ppm;
        out.best_score = rows[0].score_ppm;
        out.state = kJobDone;
        out.done_us = now_us();
        std::snprintf(out.note, sizeof out.note, "%s rows=%d stride=%d", source, n, stride);
        research_.publish(out);
    }

    void run_backtest(const JobReq& r) noexcept {
        BacktestResult out{};
        out.id = r.id;
        out.state = kJobRunning;
        out.started_us = now_us();
        backtest_.publish(out);

        std::vector<pitlane::Quote> tape;
        char source[40]{};
        load_tape(r.tape, r.name, tape, source, sizeof source);
        const int n = static_cast<int>(tape.size());

        pitlane::BtCfg cfg{};
        cfg.cash = r.cash;
        cfg.qty = r.qty;
        cfg.fee_ppm = r.fee_ppm;
        cfg.kind = r.factor_kind;
        cfg.window = r.window;
        cfg.sign = r.sign;
        cfg.threshold_ppm = r.threshold_ppm;
        cfg.cooldown_us = 0;
        cfg.stride = 1;

        pitlane::Backtester<1024> bt;
        pitlane::BtMetrics m{};
        if (n < 2 || !bt.run(tape.data(), n, cfg, m)) {
            out.state = kJobFailed;
            out.done_us = now_us();
            std::snprintf(out.note, sizeof out.note, "backtest rejected (rows=%d)", n);
            backtest_.publish(out);
            return;
        }

        out.kind = cfg.kind;
        out.window = cfg.window;
        out.sign = cfg.sign;
        out.threshold_ppm = cfg.threshold_ppm;
        out.qty = cfg.qty;
        out.fee_ppm = cfg.fee_ppm;
        out.cash = cfg.cash;
        out.points = m.points;
        out.trades = m.trades;
        out.round_trips = m.round_trips;
        out.wins = m.wins;
        out.gross_pnl = m.gross_pnl;
        out.fees = m.fees;
        out.net_pnl = m.net_pnl;
        out.turnover = m.turnover;
        out.return_ppm = m.return_ppm;
        out.annual_ppm = m.annual_ppm;
        out.sharpe_ppm = m.sharpe_ppm;
        out.max_dd_ppm = m.max_dd_ppm;
        out.win_ppm = m.win_ppm;
        out.exposure_ppm = m.exposure_ppm;
        out.final_equity = m.final_equity;
        out.span_us = m.span_us;

        pitlane::BtPoint pts[512];
        const int np = bt.curve(pts, 512);
        out.curve_n = np;
        for (int i = 0; i < np; ++i) {
            out.curve[i].ts_us = pts[i].ts_us;
            out.curve[i].equity = pts[i].equity;
            out.curve[i].dd_ppm = pts[i].dd_ppm;
        }
        out.state = kJobDone;
        out.done_us = now_us();
        if (m.span_us < 86400000000ll) {
            std::snprintf(out.note, sizeof out.note, "%s rows=%d annual=period return (span<1d)", source, n);
        } else {
            std::snprintf(out.note, sizeof out.note, "%s rows=%d", source, n);
        }
        backtest_.publish(out);
    }

    // Loads the configured tape: a block tape when present (downsampled to at
    // most 8192 quotes so cold-path jobs stay bounded), otherwise the CSV
    // loader, otherwise the deterministic synthetic tape.
    static void load_tape(const char* path, uint32_t name, std::vector<pitlane::Quote>& tape, char* note, int cap) {
        tape.clear();
        if (pitlane::tape_detail::is_block_tape(path)) {
            pitlane::BlockTapeReader r;
            pitlane::TapeMeta m{};
            if (r.open(path) && r.meta(m) && m.rows > 0) {
                constexpr uint64_t kMaxQuotes = 8192;
                if (m.rows <= kMaxQuotes) {
                    tape.resize(static_cast<std::size_t>(m.rows));
                    uint32_t n = 0;
                    uint32_t version = 0;
                    if (pitlane::tape_load_block(path, tape.data(), static_cast<uint32_t>(tape.size()), n, version) &&
                        n > 0) {
                        tape.resize(n);
                        std::snprintf(note, static_cast<std::size_t>(cap), "block v%u",
                                      static_cast<unsigned>(version));
                        return;
                    }
                } else {
                    const uint64_t stride = (m.rows + kMaxQuotes - 1) / kMaxQuotes;
                    const uint64_t want = (m.rows + stride - 1) / stride;
                    tape.resize(static_cast<std::size_t>(want));
                    pitlane::TapeTick buf[1024];
                    uint64_t src = 0;
                    uint64_t next = 0;
                    uint64_t got_total = 0;
                    while (got_total < want) {
                        uint32_t got = 0;
                        if (!r.rows(0, INT64_MIN, INT64_MAX, static_cast<uint32_t>(src), 1024, pitlane::kTapeColAll,
                                    buf, 1024, got) ||
                            got == 0) {
                            break;
                        }
                        for (uint32_t i = 0; i < got && got_total < want; ++i) {
                            if (src + i != next) continue;
                            pitlane::Quote& q = tape[static_cast<std::size_t>(got_total++)];
                            q.name = m.symbol;
                            q.exch_us = buf[i].exch_us;
                            q.local_us = buf[i].local_us;
                            q.bid_px = buf[i].bid_px;
                            q.ask_px = buf[i].ask_px;
                            q.last_px = buf[i].last_px;
                            q.bid_qty = buf[i].bid_qty;
                            q.ask_qty = buf[i].ask_qty;
                            q.last_qty = buf[i].last_qty;
                            next += stride;
                        }
                        src += got;
                    }
                    if (got_total > 0) {
                        tape.resize(static_cast<std::size_t>(got_total));
                        std::snprintf(note, static_cast<std::size_t>(cap), "block v%u stride=%llu",
                                      static_cast<unsigned>(m.versions), static_cast<unsigned long long>(stride));
                        return;
                    }
                }
            }
            tape.clear();
        }
        trading_study::TapeStats st{};
        if (trading_study::tape_load_csv(path, tape, st, name)) {
            std::snprintf(note, static_cast<std::size_t>(cap), "csv");
            return;
        }
        synth_tape(tape);
        std::snprintf(note, static_cast<std::size_t>(cap), "synthetic");
    }

    // Deterministic tape used when no file is configured.
    static void synth_tape(std::vector<pitlane::Quote>& out) {
        constexpr int kN = 600;
        out.clear();
        out.reserve(kN);
        const int64_t base = 1700000000000000ll;
        for (int i = 0; i < kN; ++i) {
            const int64_t trend = i < kN / 2 ? static_cast<int64_t>(i) * 200 : (kN - i) * 200;
            const int64_t px = 1000000 + trend + (static_cast<int64_t>(i) * 7919) % 4001 - 2000;
            pitlane::Quote q{};
            q.name = 1;
            q.exch_us = base + static_cast<int64_t>(i) * 1000000;
            q.local_us = q.exch_us;
            q.bid_px = px - 500;
            q.ask_px = px + 500;
            q.last_px = px;
            q.bid_qty = 100;
            q.ask_qty = 100;
            q.last_qty = 10;
            out.push_back(q);
        }
    }

    static int64_t now_us() noexcept {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000000ll + ts.tv_nsec / 1000;
    }

    pitlane::SendJoinRing<JobReq, kReqCap> q_{};
    SeqCell<ResearchResult> research_{};
    SeqCell<BacktestResult> backtest_{};
    std::atomic<uint64_t> next_id_{1};
    std::atomic<uint64_t> research_req_{0};
    std::atomic<uint64_t> backtest_req_{0};
    std::atomic<bool> run_{false};
    pthread_t tid_{};
    bool started_{false};
};

}  // namespace trading_jobs
