#pragma once

#include "pitlane/book/tradelane.hpp"
#include "pitlane/desk/md_engine.hpp"
#include "pitlane/desk/runtime.hpp"
#include "pitlane/study/strat_engine.hpp"
#include "pitlane/desk/td_engine.hpp"
#include "pitlane/desk/wire.hpp"
#include "pitlane/util/wirelog.hpp"

#include <cstring>

namespace pitlane {

// Reader that copies quotes onto the paper book. Strategy keeps kLoopReader.
inline constexpr unsigned kBookReader = 1;

// EngineRouteConf. Zero bits means NoFilter: every engine id passes.
struct RouteAllow {
    uint64_t bits{0};

    bool pass(uint32_t engine) const noexcept {
        if (bits == 0) return true;
        if (engine >= 64u) return false;
        return (bits & (1ull << engine)) != 0;
    }
};

// Trading-center hub. One step replaces the trading center / paper matcher process loop:
// market-data engine, quote book, strategy engine, then a live trader or the paper lane.
// Heartbeat lands on the desk pulse ring. Broker sockets stay outside.

struct Center {
    Desk* desk{nullptr};
    Engine* md{nullptr};
    TdEngine* live{nullptr};
    StratEngine* strat{nullptr};
    TradeLane* paper{nullptr};
    PaperVenue* book{nullptr};
    WireLog* log{nullptr};
    RouteAllow route{};
    uint32_t strat_engine{0};
    int64_t heartbeat_us{60000000};
    int64_t last_beat_us{0};
    uint64_t beats{0};
    unsigned quote_reader{kBookReader};
    uint64_t quote_cur{0};

    bool set_up() {
        if (!desk) return false;
        if (md) {
            md->bind_desk(desk);
            if (!md->set_up()) return false;
        }
        if (strat) {
            strat->bind_desk(desk);
            if (!route.pass(strat_engine)) return true;
            if (!strat->set_up()) return false;
        }
        if (live) {
            live->bind_desk(desk);
            if (!live->set_up()) return false;
        }
        return true;
    }

    int step(int64_t now_us) noexcept {
        if (!desk) return -1;
        if (md) md->loop_task();
        if (book) {
            Quote q{};
            bool fresh = false;
            while (desk->quotes.at(quote_cur, quote_reader, q, &fresh)) {
                if (!fresh) break;
                book->set_top(q.name, q.bid_px, q.bid_qty, q.ask_px, q.ask_qty, q.last_px);
                desk->quotes.ack(quote_cur, quote_reader);
                ++quote_cur;
            }
        }
        if (strat && route.pass(strat_engine)) strat->loop_task();
        int events = 0;
        const bool live_ready = live && live->client && live->client->ready();
        if (live_ready) live->loop_task();
        else if (paper) events = lane_beat(*paper, *desk, now_us);
        if (heartbeat_us > 0 && (last_beat_us == 0 || now_us - last_beat_us >= heartbeat_us)) {
            Pulse beat{};
            beat.stamp_us = now_us;
            beat.value = static_cast<int64_t>(beats + 1);
            std::memcpy(beat.topic, "hb", 3);
            if (desk->pulses.publish(beat, 0)) {
                ++beats;
                last_beat_us = now_us;
            }
        }
        return events;
    }
};

} // namespace pitlane
