#include "pitlane/net/frame.hpp"
#include "pitlane/net/hub.hpp"
#include "pitlane/net/logbuf.hpp"
#include "pitlane/net/replicate.hpp"
#include "pitlane/net/udp.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_fails = 0;

static void expect(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "fail: %s\n", what);
    ++g_fails;
}

static void check_frame() {
    pitlane::Report rep{};
    rep.tag = 7;
    rep.contra = 3;
    rep.px = 100;
    rep.qty = 5;
    rep.left = 1;
    rep.stamp_us = 1234;
    rep.why = 0;
    rep.phase = 2;
    rep.side = 1;
    rep.name = 42;

    uint8_t buf[128]{};
    std::size_t n = 0;
    expect(pitlane::net_pack(pitlane::NetKind::Report, 9, 1000, rep, buf, sizeof(buf), &n), "pack report");
    expect(n == 32u + sizeof(pitlane::Report), "pack length");
    expect(pitlane::net_crc32c(&rep, sizeof(rep)) != 0, "crc nonzero");

    pitlane::NetHeader head{};
    pitlane::Report out{};
    std::size_t used = 0;
    expect(pitlane::net_unpack(buf, n, head, out, &used), "unpack report");
    expect(used == n && head.kind == static_cast<uint8_t>(pitlane::NetKind::Report) && head.seq == 9 &&
               head.stamp_us == 1000 && head.magic == pitlane::kNetMagic && head.version == pitlane::kNetVersion,
           "report header");
    expect(out.tag == 7 && out.contra == 3 && out.px == 100 && out.qty == 5 && out.left == 1 && out.side == 1 &&
               out.name == 42,
           "report body");

    pitlane::Intent intent{};
    intent.tag = 11;
    intent.name = 5;
    intent.acct = 6;
    intent.px = 50;
    intent.qty = 2;
    intent.stamp_us = 77;
    intent.side = 2;
    intent.act = 1;
    intent.style = 1;
    intent.lot = 9;

    uint8_t ibuf[128]{};
    std::size_t in = 0;
    expect(pitlane::net_pack(pitlane::NetKind::Intent, 12, 2000, intent, ibuf, sizeof(ibuf), &in), "pack intent");
    pitlane::NetHeader ihead{};
    pitlane::Intent iout{};
    std::size_t iused = 0;
    expect(pitlane::net_unpack(ibuf, in, ihead, iout, &iused), "unpack intent");
    expect(ihead.kind == static_cast<uint8_t>(pitlane::NetKind::Intent) && ihead.seq == 12 && iout.tag == 11 &&
               iout.side == 2 && iout.px == 50 && iout.lot == 9,
           "intent body");

    uint8_t bad[128]{};
    std::memcpy(bad, buf, n);
    bad[4] ^= 1u;
    pitlane::Report keep{};
    keep.tag = 1234;
    std::size_t bad_used = 0;
    expect(!pitlane::net_unpack(bad, n, head, keep, &bad_used) && keep.tag == 1234, "bad crc rejected");

    std::memcpy(bad, buf, n);
    bad[28] ^= 0xffu;
    expect(!pitlane::net_unpack(bad, n, head, keep, &bad_used) && keep.tag == 1234, "bad magic rejected");

    expect(!pitlane::net_unpack(buf, 31, head, keep, &bad_used), "short header rejected");
    expect(!pitlane::net_unpack(buf, n - 1, head, keep, &bad_used), "short frame rejected");

    expect(!pitlane::net_pack(pitlane::NetKind::Report, 1, 1, rep, nullptr, sizeof(buf), &n), "null out");
    expect(!pitlane::net_pack(pitlane::NetKind::Report, 1, 1, rep, buf, 32u + sizeof(rep) - 1u, &n), "small cap");
    expect(!pitlane::net_pack(static_cast<pitlane::NetKind>(0), 1, 1, rep, buf, sizeof(buf), &n), "bad kind");
    expect(!pitlane::net_pack(pitlane::NetKind::Report, 1, 1, rep, buf, sizeof(buf), nullptr), "null written");
}

static void check_log() {
    using Log = pitlane::TermLog<1024, 3>;
    uint8_t pay[256]{};
    for (int i = 0; i < 256; ++i) pay[i] = static_cast<uint8_t>(i);

    Log log;
    expect(log.tail() == 0 && log.min_reader() == 0, "empty log");
    expect(log.reader_pos(0) == 0 && log.reader_pos(9) == 0, "invalid reader pos");
    expect(log.add_reader() == 0 && log.add_reader() == 1, "two readers");
    expect(log.add_reader() >= 0, "third reader");

    for (int i = 0; i < 8; ++i) {
        pay[0] = static_cast<uint8_t>(i);
        expect(log.publish(pay, 100), "wrap publish");
    }
    expect(log.tail() == 1344, "wrap tail");

    uint8_t got[256]{};
    for (int r = 0; r < 3; ++r) {
        for (int i = 0; i < 8; ++i) {
            uint32_t len = 0;
            uint16_t type = 0;
            expect(log.poll(r, got, sizeof(got), len, type) == 1, "wrap poll");
            expect(len == 100 && type == 1 && got[0] == static_cast<uint8_t>(i), "wrap body");
        }
        uint32_t len = 0;
        uint16_t type = 0;
        expect(log.poll(r, got, sizeof(got), len, type) == 0, "wrap done");
    }

    Log pad;
    expect(pad.add_reader() == 0, "pad reader");
    expect(pad.publish(nullptr, 0), "empty frame allowed");
    uint32_t plen = 0;
    uint16_t ptype = 0;
    expect(pad.poll(0, got, 4, plen, ptype) == 1 && plen == 0 && ptype == 1, "empty frame poll");

    Log limits;
    expect(limits.set_flow_limit(1024), "flow set");
    expect(!limits.set_flow_limit(1023), "flow below term");
    expect(!limits.set_flow_limit(1024 * 3), "flow covers all terms");
    expect(limits.set_flow_limit(1024), "flow set again");
    const int fr = limits.add_reader();
    expect(fr == 0, "flow reader");
    for (int i = 0; i < 4; ++i) expect(limits.publish(pay, 200), "flow publish");
    const uint64_t before = limits.tail();
    expect(!limits.publish(pay, 200), "flow blocks");
    expect(limits.tail() == before, "flow leaves tail");
    expect(limits.min_reader() == 0, "flow min reader");
    limits.resync(fr);
    expect(limits.reader_pos(fr) == before, "resync reader");
    expect(limits.publish(pay, 200), "flow resumes");

    Log bad;
    expect(!bad.publish(nullptr, 4), "null payload rejected");
    expect(!bad.publish(pay, 961), "oversized payload rejected");
    expect(!bad.publish(pay, 4, static_cast<uint16_t>(pitlane::LogFrameType::Pad)), "pad publish rejected");

    Log maxlog;
    uint8_t maxp[960]{};
    expect(maxlog.publish(maxp, 960), "max payload");

    Log small;
    expect(small.add_reader() == 0, "small reader");
    expect(small.publish(pay, 16), "small publish");
    uint8_t tiny[4]{};
    uint32_t slen = 0;
    uint16_t stype = 0;
    expect(small.poll(0, tiny, 4, slen, stype) == -1, "small cap rejected");
    small.resync(0);
    expect(small.poll(0, tiny, 4, slen, stype) == 0, "resync drained");
    expect(small.poll(9, tiny, 4, slen, stype) == 0, "invalid reader");

    Log beat;
    expect(beat.add_reader() == 0, "beat reader");
    expect(beat.publish(pay, 8, static_cast<uint16_t>(pitlane::LogFrameType::Heartbeat)), "heartbeat publish");
    expect(beat.poll(0, got, sizeof(got), slen, stype) == 1 && stype == 3 && slen == 8, "heartbeat poll");
}

static void check_replica() {
    using pitlane::ReplicaRole;

    pitlane::ReplicaLog<int, 16> log;
    expect(log.role() == ReplicaRole::Follower && log.last_index() == 0 && log.last_term() == 0, "follower start");
    expect(!log.leader_append(1, 0), "follower cannot append");

    int vals[3] = {10, 20, 30};
    expect(log.follower_append(1, 0, 0, vals, 3, 2) == 3, "append three");
    expect(log.last_index() == 3 && log.last_term() == 1 && log.commit_index() == 2 && log.apply_index() == 0,
           "append state");

    int out[8]{};
    expect(log.apply(out, 8) == 2 && out[0] == 10 && out[1] == 20 && log.apply_index() == 2, "apply committed");
    expect(log.apply(out, 8) == 0, "nothing new");

    int more[1] = {40};
    expect(log.follower_append(1, 3, 1, more, 1, 3) == 1 && log.commit_index() == 3, "append fourth");
    expect(log.apply(out, 8) == 1 && out[0] == 30, "apply third");

    int bad[1] = {99};
    expect(log.follower_append(1, 3, 9, bad, 1, 0) == -1, "prev term mismatch");
    expect(log.follower_append(1, 9, 0, bad, 1, 0) == -1, "prev index gap");
    expect(log.follower_append(0, 4, 1, bad, 1, 0) == -1, "term regression");
    expect(log.follower_append(1, 4, 1, nullptr, 1, 0) == -1, "null entries");
    expect(log.follower_append(1, 4, 1, bad, -1, 0) == -1, "negative n");

    pitlane::ReplicaLog<int, 8> small;
    int eight[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    expect(small.follower_append(1, 0, 0, eight, 8, 0) == 8, "fill cap");
    expect(small.follower_append(1, 8, 1, eight, 1, 0) == -1, "over cap");
    expect(small.last_index() == 8, "over cap unchanged");

    pitlane::ReplicaLog<int, 16> tr;
    int one[1] = {1};
    int two[2] = {2, 3};
    expect(tr.follower_append(1, 0, 0, one, 1, 0) == 1, "truncate seed");
    expect(tr.follower_append(1, 1, 1, two, 2, 0) == 2 && tr.last_index() == 3, "truncate append");
    int conflict[1] = {7};
    expect(tr.follower_append(2, 1, 1, conflict, 1, 0) == 1 && tr.last_index() == 2 && tr.last_term() == 2,
           "conflict truncates");

    pitlane::ReplicaLog<int, 16> capsmall;
    int three[3] = {1, 2, 3};
    expect(capsmall.follower_append(1, 0, 0, three, 3, 3) == 3, "cap seed");
    int out2[2]{};
    expect(capsmall.apply(out2, 2) == 0 && capsmall.apply_index() == 0, "apply cap too small");

    pitlane::ReplicaLog<int, 16> lead;
    lead.become_leader(5, 2);
    expect(lead.role() == ReplicaRole::Leader && lead.term() == 5, "become leader");
    expect(lead.leader_append(42, 0), "leader append");
    expect(lead.last_index() == 1 && lead.last_term() == 5, "leader entry");
    lead.match(1, 1);
    lead.match(2, 0);
    expect(lead.commit_quorum() == 1, "quorum two of three");

    pitlane::ReplicaLog<int, 16> q3;
    int qv[2] = {5, 6};
    expect(q3.follower_append(1, 0, 0, qv, 2, 0) == 2, "q3 seed");
    q3.become_leader(2, 3);
    q3.match(1, 2);
    expect(q3.commit_quorum() == 0, "three way needs all");
    q3.match(2, 1);
    expect(q3.commit_quorum() == 1, "three way slowest");

    pitlane::ReplicaLog<int, 16> follows;
    follows.match(0, 5);
    expect(follows.commit_quorum() == 0, "follower no commit");
}

static void check_udp() {
    pitlane::UdpAddr a{};
    pitlane::UdpAddr b{};
    expect(pitlane::udp_addr("127.0.0.1", 8080, a), "addr parse");
    expect(pitlane::udp_parse("127.0.0.1:8080", b), "text parse");
    expect(a.ip_be == b.ip_be && b.port == 8080, "text parse fields");

    pitlane::UdpAddr c{};
    expect(pitlane::udp_parse("10.1.2.3", c) && c.port == 0, "text parse no port");
    expect(!pitlane::udp_parse("", c), "empty text");
    expect(!pitlane::udp_parse("1.2.3", c), "short ip");
    expect(!pitlane::udp_parse("1.2.3.4:", c), "missing port");
    expect(!pitlane::udp_parse("1.2.3.4:70000", c), "port overflow");
    expect(!pitlane::udp_parse("1.2.3.4:x", c), "bad port");
    expect(!pitlane::udp_parse("999.2.3.4", c), "bad ip");

    pitlane::UdpLink left;
    pitlane::UdpLink right;
    pitlane::UdpAddr bind{};
    expect(pitlane::udp_addr("127.0.0.1", 0, bind), "bind addr");
    expect(left.open(bind) && right.open(bind), "open links");
    expect(left.fd() >= 0 && right.fd() >= 0, "open fds");
    const uint16_t lp = left.local_port();
    const uint16_t rp = right.local_port();
    expect(lp != 0 && rp != 0 && lp != rp, "ephemeral ports");

    pitlane::UdpAddr lp_addr{};
    pitlane::UdpAddr rp_addr{};
    expect(pitlane::udp_addr("127.0.0.1", lp, lp_addr) && pitlane::udp_addr("127.0.0.1", rp, rp_addr), "peer addrs");
    expect(left.set_peer(rp_addr) && right.set_peer(lp_addr), "connect peers");

    expect(left.send("ping", 4) == 4, "send ping");
    uint8_t got[16]{};
    int n = -1;
    for (int i = 0; i < 10000 && n <= 0; ++i) n = right.recv(got, sizeof(got));
    expect(n == 4 && std::memcmp(got, "ping", 4) == 0, "recv ping");

    right.close();
    expect(right.fd() < 0 && right.send("x", 1) == 0 && right.recv(got, sizeof(got)) == 0, "closed link");
}

static void check_hub() {
    static pitlane::Desk desk{};
    using Hub = pitlane::RemoteHub<1024, 3>;

    pitlane::TermLog<1024, 3> outlog;
    pitlane::TermLog<1024, 3> inlog;
    Hub hub;
    hub.desk = &desk;
    hub.out = &outlog;
    hub.in = &inlog;
    hub.in_reader = inlog.add_reader();
    hub.out_reader = outlog.add_reader();
    expect(hub.in_reader == 0 && hub.out_reader == 0, "hub readers");

    pitlane::Report rep{};
    rep.tag = 77;
    rep.contra = 1;
    rep.px = 100;
    rep.qty = 2;
    rep.left = 0;
    rep.stamp_us = 555;
    rep.why = 0;
    rep.phase = 2;
    rep.side = 1;
    rep.name = 9;
    expect(desk.reports.publish(rep, 1ull << hub.local_reader), "desk report");
    expect(hub.pump(1000) == 0, "pump reports only");

    uint8_t buf[256]{};
    uint32_t len = 0;
    uint16_t type = 0;
    expect(outlog.poll(hub.out_reader, buf, static_cast<uint32_t>(sizeof(buf)), len, type) == 1 && type == 1,
           "report frame");
    pitlane::NetHeader head{};
    pitlane::Report decoded{};
    std::size_t used = 0;
    expect(pitlane::net_unpack<pitlane::Report>(buf, len, head, decoded, &used), "report decode");
    expect(head.kind == static_cast<uint8_t>(pitlane::NetKind::Report) && head.seq == 1 && decoded.tag == 77 &&
               decoded.px == 100,
           "report fields");

    pitlane::Quote q{};
    q.name = 3;
    q.bid_px = 99;
    q.ask_px = 101;
    q.local_us = 0;
    expect(desk.quotes.publish(q, 1ull << hub.local_reader), "desk quote");
    expect(hub.pump(2000) == 0, "pump quotes only");
    expect(outlog.poll(hub.out_reader, buf, static_cast<uint32_t>(sizeof(buf)), len, type) == 1 && type == 1,
           "quote frame");
    pitlane::Quote qdecoded{};
    expect(pitlane::net_unpack<pitlane::Quote>(buf, len, head, qdecoded, &used), "quote decode");
    expect(head.kind == static_cast<uint8_t>(pitlane::NetKind::Quote) && head.seq == 2 && qdecoded.bid_px == 99 &&
               qdecoded.ask_px == 101,
           "quote fields");

    pitlane::Intent intent{};
    intent.tag = 99;
    intent.name = 4;
    intent.acct = 5;
    intent.px = 50;
    intent.qty = 3;
    intent.stamp_us = 777;
    intent.side = 2;
    intent.act = 1;
    intent.style = 1;
    intent.lot = 1;
    uint8_t frame[256]{};
    std::size_t frame_len = 0;
    expect(pitlane::net_pack<pitlane::Intent>(pitlane::NetKind::Intent, 1, 777, intent, frame, sizeof(frame),
                                              &frame_len),
           "pack intent");
    expect(inlog.publish(frame, static_cast<uint32_t>(frame_len), 1), "in log intent");
    expect(hub.pump(3000) == 1, "inbound accepted");

    pitlane::Intent got{};
    expect(desk.intents.poll(0, pitlane::kGateReader, got, false) == 1, "gate sees intent");
    desk.intents.ack(0, pitlane::kGateReader);
    expect(got.tag == 99 && got.side == 2 && got.qty == 3 && got.px == 50, "intent fields");
    expect(hub.pump(4000) == 0, "no duplicate intent");
}

static void check_hub_udp() {
    static pitlane::Desk desk{};
    using Hub = pitlane::RemoteHub<1024, 3>;

    pitlane::TermLog<1024, 3> outlog;
    pitlane::TermLog<1024, 3> inlog;
    Hub hub;
    hub.desk = &desk;
    hub.out = &outlog;
    hub.in = &inlog;
    hub.local_reader = 3;
    hub.in_reader = inlog.add_reader();
    hub.out_reader = outlog.add_reader();

    pitlane::UdpLink local;
    pitlane::UdpLink remote;
    pitlane::UdpAddr bind{};
    expect(pitlane::udp_addr("127.0.0.1", 0, bind), "hub bind addr");
    expect(local.open(bind) && remote.open(bind), "hub links");
    pitlane::UdpAddr lp{};
    pitlane::UdpAddr rp{};
    expect(pitlane::udp_addr("127.0.0.1", local.local_port(), lp) &&
               pitlane::udp_addr("127.0.0.1", remote.local_port(), rp),
           "hub peer addrs");
    expect(local.set_peer(rp) && remote.set_peer(lp), "hub peers");

    pitlane::Intent intent{};
    intent.tag = 123;
    intent.name = 8;
    intent.acct = 9;
    intent.px = 44;
    intent.qty = 6;
    intent.stamp_us = 888;
    intent.side = 1;
    intent.act = 1;
    intent.style = 1;
    intent.lot = 1;
    uint8_t frame[256]{};
    std::size_t frame_len = 0;
    expect(pitlane::net_pack<pitlane::Intent>(pitlane::NetKind::Intent, 1, 888, intent, frame, sizeof(frame),
                                              &frame_len),
           "hub pack intent");
    expect(remote.send(frame, frame_len) == static_cast<int>(frame_len), "hub send intent");

    int accepted = 0;
    for (int i = 0; i < 10000 && accepted == 0; ++i) accepted = pitlane::hub_udp_pump(hub, local, local, 1000);
    expect(accepted >= 1, "udp hub accepted");

    pitlane::Intent got{};
    expect(desk.intents.poll(0, pitlane::kGateReader, got, false) == 1, "udp gate sees intent");
    desk.intents.ack(0, pitlane::kGateReader);
    expect(got.tag == 123 && got.qty == 6, "udp intent fields");

    pitlane::Report rep{};
    rep.tag = 321;
    rep.px = 21;
    rep.qty = 4;
    rep.side = 1;
    rep.stamp_us = 999;
    expect(desk.reports.publish(rep, 1ull << hub.local_reader), "udp desk report");
    expect(hub.pump(2000) >= 0, "udp pump desk");
    expect(pitlane::hub_udp_pump(hub, local, local, 2000) >= 0, "udp hub send report");

    uint8_t got_frame[256]{};
    int n = -1;
    for (int i = 0; i < 10000 && n <= 0; ++i) n = remote.recv(got_frame, sizeof(got_frame));
    expect(n > 0, "udp report received");
    pitlane::NetHeader head{};
    pitlane::Report decoded{};
    std::size_t used = 0;
    expect(n > 0 && pitlane::net_unpack<pitlane::Report>(got_frame, static_cast<std::size_t>(n), head, decoded, &used),
           "udp report decode");
    expect(head.kind == static_cast<uint8_t>(pitlane::NetKind::Report) && decoded.tag == 321 && decoded.px == 21,
           "udp report fields");
}

int main() {
    check_frame();
    check_log();
    check_replica();
    check_udp();
    check_hub();
    check_hub_udp();
    if (g_fails) {
        std::fprintf(stderr, "%d net check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
