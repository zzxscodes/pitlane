#pragma once

#include "pitlane/desk/region.hpp"
#include "pitlane/desk/runtime.hpp"
#include "pitlane/desk/types.hpp"
#include "pitlane/desk/wire.hpp"
#include "pitlane/net/frame.hpp"
#include "pitlane/net/logbuf.hpp"
#include "pitlane/net/udp.hpp"
#include "pitlane/util/hint.hpp"

#include <cstddef>
#include <cstdint>

// Bridges the desk rings to a term log. Outbound reports and quotes are packed
// into the out log; inbound intent frames from the in log are decoded and
// published to the trader gateway lane. One caller owns the clock.

namespace pitlane {

template <uint32_t TermLen = 65536, uint32_t Terms = 3>
struct RemoteHub {
    Desk* desk{nullptr};
    TermLog<TermLen, Terms>* out{nullptr};
    TermLog<TermLen, Terms>* in{nullptr};
    unsigned local_reader{2};
    int in_reader{-1};
    int out_reader{-1};
    uint64_t quote_cur{0};
    uint64_t report_cur{0};
    uint64_t seq{0};

    PITLANE_HOT int pump(int64_t now_us) noexcept {
        uint8_t buf[2048];

        if (desk && out) {
            auto push_report = [&](const Report& r) noexcept {
                std::size_t n = 0;
                if (net_pack<Report>(NetKind::Report, ++seq, now_us, r, buf, sizeof(buf), &n))
                    out->publish(buf, static_cast<uint32_t>(n), 1);
            };
            ring_take(desk->reports, report_cur, local_reader, false, push_report);

            auto push_quote = [&](const Quote& q) noexcept {
                std::size_t n = 0;
                if (net_pack<Quote>(NetKind::Quote, ++seq, now_us, q, buf, sizeof(buf), &n))
                    out->publish(buf, static_cast<uint32_t>(n), 1);
            };
            ring_take(desk->quotes, quote_cur, local_reader, false, push_quote);
        }

        int accepted = 0;
        if (desk && in && in_reader >= 0) {
            for (;;) {
                uint32_t len = 0;
                uint16_t type = 0;
                const int st = in->poll(in_reader, buf, static_cast<uint32_t>(sizeof(buf)), len, type);
                if (st <= 0) break;
                if (type != static_cast<uint16_t>(LogFrameType::Data)) continue;
                NetHeader head{};
                Intent intent{};
                std::size_t used = 0;
                if (!net_unpack<Intent>(buf, len, head, intent, &used)) continue;
                if (head.kind != static_cast<uint8_t>(NetKind::Intent)) continue;
                if (desk->intents.publish(intent, 1ull << kGateReader)) ++accepted;
            }
        }
        return accepted;
    }
};

// Moves datagrams into the in log, drains the out log into tx, then pumps the
// desk rings. Returns a non-negative count of datagrams and intents moved.
template <uint32_t TermLen, uint32_t Terms>
PITLANE_HOT int hub_udp_pump(RemoteHub<TermLen, Terms>& hub, UdpLink& rx, UdpLink& tx, int64_t now_us) noexcept {
    uint8_t buf[2048];
    int moved = 0;

    for (;;) {
        const int n = rx.recv(buf, sizeof(buf));
        if (n <= 0) break;
        if (hub.in && hub.in->publish(buf, static_cast<uint32_t>(n))) {
            ++moved;
        } else {
            break;
        }
    }

    if (hub.out && hub.out_reader >= 0) {
        for (;;) {
            uint32_t len = 0;
            uint16_t type = 0;
            const int st = hub.out->poll(hub.out_reader, buf, static_cast<uint32_t>(sizeof(buf)), len, type);
            if (st <= 0) break;
            if (type != static_cast<uint16_t>(LogFrameType::Data)) continue;
            if (tx.send(buf, len) < 0) break;
            ++moved;
        }
    }

    moved += hub.pump(now_us);
    return moved;
}

}  // namespace pitlane
