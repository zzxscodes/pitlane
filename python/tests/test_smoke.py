"""Smoke tests for the ctypes wrapper over libpitlane_c.so (stdlib unittest).

Run from the repository root after the C ABI target is built::

    PYTHONPATH=python python3 python/tests/test_smoke.py
"""

import os
import shutil
import tempfile
import unittest

import pitlane

BASE_US = 1700000000000000
STEP_US = 1000
ROWS = 5000


def make_ticks(n=ROWS):
    ticks = []
    for i in range(n):
        exch = BASE_US + i * STEP_US
        bid = 100000 + (i % 100) * 10
        ticks.append((exch, exch + 5, bid, bid + 10, bid + 5, 1 + i % 7, 2 + i % 5, 1 + i % 3))
    return ticks


class TapeSmokeTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="pitlane-tape-")
        self.path = os.path.join(self.dir, "ticks.plt")
        self.ticks = make_ticks()
        pitlane.write_tape(self.path, self.ticks, symbol=7)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_meta(self):
        with pitlane.Tape.open(self.path) as tape:
            meta = tape.meta()
        self.assertEqual(meta["rows"], ROWS)
        self.assertEqual(meta["versions"], 1)
        self.assertEqual(meta["symbol"], 7)
        self.assertEqual(meta["blocks"], (ROWS + 1023) // 1024)
        self.assertEqual(meta["first_ts"], BASE_US)
        self.assertEqual(meta["last_ts"], BASE_US + (ROWS - 1) * STEP_US)
        self.assertGreater(meta["bytes"], 0)

    def test_rows_limit(self):
        with pitlane.Tape.open(self.path) as tape:
            rows = tape.rows(limit=10)
        self.assertEqual(len(rows), 10)
        self.assertEqual(rows[0].exch_us, BASE_US)
        self.assertEqual(rows[-1].exch_us, BASE_US + 9 * STEP_US)
        self.assertEqual(rows[3].as_tuple(), self.ticks[3])

    def test_time_range(self):
        since = BASE_US + 1000 * STEP_US
        until = BASE_US + 1999 * STEP_US
        with pitlane.Tape.open(self.path) as tape:
            rows = tape.rows(since_us=since, until_us=until, limit=0)
            page = tape.rows(since_us=since, until_us=until, offset=10, limit=5)
        self.assertEqual(len(rows), 1000)
        self.assertEqual(rows[0].exch_us, since)
        self.assertEqual(rows[-1].exch_us, until)
        self.assertEqual([t.exch_us for t in page], [since + k * STEP_US for k in range(10, 15)])

    def test_describe(self):
        with pitlane.Tape.open(self.path) as tape:
            cols = tape.describe()
        self.assertEqual(len(cols), 8)
        self.assertEqual(
            [c["name"] for c in cols],
            [
                "exch_us",
                "local_us",
                "bid_px",
                "ask_px",
                "last_px",
                "bid_qty",
                "ask_qty",
                "last_qty",
            ],
        )
        for c in cols:
            self.assertEqual(c["rows"], ROWS)
        expected_last = sum(t[4] for t in self.ticks) / float(ROWS)
        self.assertAlmostEqual(cols[4]["mean"], expected_last, places=6)
        self.assertEqual(cols[4]["min"], min(t[4] for t in self.ticks))
        self.assertEqual(cols[4]["max"], max(t[4] for t in self.ticks))

    def test_backtest(self):
        metrics = pitlane.backtest(self.path)
        self.assertIsInstance(metrics, dict)
        self.assertGreaterEqual(metrics["points"], 2)
        self.assertEqual(metrics["net_pnl"], metrics["final_equity"] - 1000000)

    def test_open_missing(self):
        missing = os.path.join(self.dir, "missing.plt")
        with self.assertRaises((RuntimeError, OSError)):
            pitlane.Tape.open(missing)

    def test_open_truncated(self):
        truncated = os.path.join(self.dir, "truncated.plt")
        with open(self.path, "rb") as src:
            head = src.read(1000)
        with open(truncated, "wb") as dst:
            dst.write(head)
        with self.assertRaises((RuntimeError, OSError)):
            pitlane.Tape.open(truncated)


class StrategyBridgeTest(unittest.TestCase):
    def test_event_roundtrip(self):
        with pitlane.Bridge(event_cap=8, cmd_cap=4) as bridge:
            self.assertTrue(
                bridge.post(
                    pitlane.QUOTE, name=7, px=101, qty=3, a=102, b=4, stamp_us=11, seq=1
                )
            )
            self.assertEqual(bridge.events_pending, 1)
            events = bridge.poll(8)
            self.assertEqual(bridge.events_pending, 0)
        self.assertEqual(len(events), 1)
        ev = events[0]
        self.assertEqual(
            (ev.kind, ev.name, ev.px, ev.qty, ev.a, ev.b, ev.stamp_us, ev.seq),
            (pitlane.QUOTE, 7, 101, 3, 102, 4, 11, 1),
        )
        self.assertEqual(ev.label, "quote")
        self.assertEqual(pitlane.Event(kind=pitlane.TIMER, seq=3).kind, pitlane.TIMER)

    def test_dispatch_and_orders(self):
        class Recorder(pitlane.Strategy):
            def __init__(self, bridge):
                super().__init__(bridge)
                self.px = []

            def on_quote(self, ev):
                self.px.append(ev.px)
                self.ctx.insert(ev.name, 1, ev.px, 1)

        with pitlane.Bridge(event_cap=16, cmd_cap=8) as bridge:
            strat = Recorder(bridge)
            self.assertTrue(bridge.post(pitlane.QUOTE, name=7, px=100))
            self.assertTrue(bridge.post(pitlane.TIMER, stamp_us=5))
            self.assertEqual(pitlane.pump(strat, bridge), 2)
            self.assertEqual(strat.px, [100])
            cmd = bridge.take_cmd()
            self.assertIsNotNone(cmd)
            self.assertEqual(
                (cmd.kind, cmd.name, cmd.px, cmd.qty, cmd.side),
                (pitlane.INSERT, 7, 100, 1, 1),
            )
            self.assertIsNone(bridge.take_cmd())

    def test_run_stops_on_signal(self):
        class Once(pitlane.Strategy):
            def __init__(self, bridge):
                super().__init__(bridge)
                self.stopped_in_handler = False

            def on_quote(self, ev):
                self.ctx.insert(ev.name, 2, ev.px, 1)
                self.stop()

        with pitlane.Bridge() as bridge:
            strat = Once(bridge)
            bridge.post(pitlane.QUOTE, name=3, px=99)
            self.assertEqual(pitlane.run(strat, bridge, idle_sleep=0), 1)
            cmd = bridge.take_cmd()
            self.assertEqual((cmd.name, cmd.side, cmd.px), (3, 2, 99))

    def test_overflow_is_counted(self):
        with pitlane.Bridge(event_cap=2, cmd_cap=2) as bridge:
            self.assertTrue(bridge.post(pitlane.TIMER, seq=1))
            self.assertTrue(bridge.post(pitlane.TIMER, seq=2))
            self.assertFalse(bridge.post(pitlane.TIMER, seq=3))
            self.assertEqual(bridge.events_lost, 1)
            self.assertTrue(bridge.send(pitlane.INSERT, name=1, px=1, qty=1))
            self.assertTrue(bridge.send(pitlane.INSERT, name=2, px=1, qty=1))
            self.assertFalse(bridge.send(pitlane.INSERT, name=3, px=1, qty=1))
            self.assertEqual(bridge.cmds_lost, 1)
            self.assertEqual(bridge.cmds_pending, 2)

    def test_threaded_handoff(self):
        import threading

        class Sink(pitlane.Strategy):
            def __init__(self, bridge):
                super().__init__(bridge)
                self.seq = []

            def on_timer(self, ev):
                self.seq.append(ev.seq)

        with pitlane.Bridge(event_cap=1024, cmd_cap=64) as bridge:
            sink = Sink(bridge)

            def produce():
                for i in range(500):
                    while not bridge.post(pitlane.TIMER, seq=i, stamp_us=i):
                        pass

            worker = threading.Thread(target=produce)
            worker.start()
            while worker.is_alive() or bridge.events_pending:
                pitlane.pump(sink, bridge, 64)
            worker.join()
            self.assertEqual(bridge.events_lost, 0)
        self.assertEqual(sink.seq, list(range(500)))

    def test_context_kinds(self):
        with pitlane.Bridge(cmd_cap=32) as bridge:
            ctx = pitlane.Context(bridge, "probe")
            self.assertTrue(ctx.cancel(9))
            self.assertTrue(ctx.pause())
            self.assertTrue(ctx.resume())
            self.assertTrue(ctx.subscribe(4))
            self.assertTrue(ctx.unsubscribe(4))
            self.assertTrue(ctx.query_asset(11))
            self.assertTrue(ctx.query_position(5, 12))
            self.assertTrue(ctx.signal_sub(6, 13))
            self.assertTrue(ctx.signal_unsub(6, 13))
            self.assertTrue(ctx.insert_algo(7, 1, 100, 2, seq=1, a=3, b=4))
            self.assertTrue(ctx.cancel_algo(15, 7))
            self.assertTrue(ctx.subscribe_mkt_order([1, 2]))
            self.assertTrue(ctx.subscribe_mkt_trade(3))
            self.assertTrue(ctx.subscribe_rebuild(4))
            self.assertTrue(ctx.query_source(16, 2))
            self.assertTrue(ctx.set_log_state(3))
            self.assertTrue(ctx.control_rsp(17, 1, 8))
            kinds = []
            for _ in range(18):
                cmd = bridge.take_cmd()
                self.assertIsNotNone(cmd)
                kinds.append(cmd.kind)
            self.assertEqual(
                kinds,
                [
                    pitlane.CANCEL,
                    pitlane.PAUSE,
                    pitlane.RESUME,
                    pitlane.SUBSCRIBE,
                    pitlane.UNSUBSCRIBE,
                    pitlane.QUERY_ASSET,
                    pitlane.QUERY_POSITION,
                    pitlane.SIGNAL_SUB,
                    pitlane.SIGNAL_UNSUB,
                    pitlane.INSERT_ALGO,
                    pitlane.CANCEL_ALGO,
                    pitlane.SUB_MKT_ORDER,
                    pitlane.SUB_MKT_ORDER,
                    pitlane.SUB_MKT_TRADE,
                    pitlane.SUB_REBUILD,
                    pitlane.QUERY_SOURCE,
                    pitlane.SET_LOG_STATE,
                    pitlane.CONTROL_RSP_CMD,
                ],
            )

    def test_event_taxonomy(self):
        kinds = [
            pitlane.QUOTE,
            pitlane.ORDER,
            pitlane.TRADE,
            pitlane.SIGNAL,
            pitlane.ASSET,
            pitlane.POSITION,
            pitlane.PACKAGE_REQ,
            pitlane.PACKAGE_RSP,
            pitlane.TIMER,
            pitlane.REBUILD_QUOTE,
            pitlane.MKT_ORDER,
            pitlane.MKT_TRADE,
            pitlane.ALGO_REQ,
            pitlane.ALGO_RSP,
            pitlane.SIGNAL_BATCH,
            pitlane.CONTROL_REQ,
            pitlane.CONTROL_RSP,
            pitlane.PROPERTY,
            pitlane.LOG,
            pitlane.NEW_PARAM,
            pitlane.ONE_PARAM_DONE,
            pitlane.NEW_DATE,
            pitlane.ONE_DATE_DONE,
            pitlane.BACKTEST_DONE,
        ]
        self.assertEqual(sorted(kinds), list(range(1, 25)))
        self.assertEqual(sorted(pitlane.strategy._EVENT_NAMES), kinds)

        hits = []

        class Probe(pitlane.Strategy):
            def on_rebuild_quote(self, ev):
                hits.append(ev.kind)

            def on_mkt_order(self, ev):
                hits.append(ev.kind)

            def on_algo_rsp(self, ev):
                hits.append(ev.kind)

            def on_backtest_done(self, ev):
                hits.append(ev.kind)

        probe = Probe()
        for kind in (pitlane.REBUILD_QUOTE, pitlane.MKT_ORDER, pitlane.ALGO_RSP, pitlane.BACKTEST_DONE):
            probe.on_event(pitlane.Event(kind=kind))
        self.assertEqual(
            hits,
            [pitlane.REBUILD_QUOTE, pitlane.MKT_ORDER, pitlane.ALGO_RSP, pitlane.BACKTEST_DONE],
        )
        self.assertGreater(pitlane.now_us(), 0)

    def test_log_lane(self):
        with pitlane.Bridge() as bridge:
            strat = pitlane.Strategy(bridge)
            self.assertTrue(strat.info("hello"))
            self.assertTrue(bridge.log(pitlane.LOG_ERROR, "bad"))
            self.assertEqual(bridge.logs_pending, 2)
            first = bridge.take_log()
            self.assertEqual((first.level, first.label, first.text), (pitlane.LOG_INFO, "info", "hello"))
            self.assertGreater(first.stamp_us, 0)
            second = bridge.take_log()
            self.assertEqual((second.level, second.label, second.text), (pitlane.LOG_ERROR, "error", "bad"))
            self.assertIsNone(bridge.take_log())

            long_text = "x" * (pitlane.LOG_TEXT + 20)
            self.assertTrue(bridge.log(pitlane.LOG_TRACE, long_text))
            self.assertEqual(len(bridge.take_log().text), pitlane.LOG_TEXT - 1)

            for _ in range(1024):
                self.assertTrue(bridge.log(pitlane.LOG_DEBUG, "f"))
            self.assertFalse(bridge.log(pitlane.LOG_DEBUG, "f"))
            self.assertEqual(bridge.logs_lost, 1)

    def test_detached_log_is_dropped(self):
        strat = pitlane.Strategy()
        self.assertFalse(strat.info("nobody"))


class VersionTest(unittest.TestCase):
    def test_version(self):
        self.assertIsInstance(pitlane.version(), str)
        self.assertNotEqual(pitlane.version(), "")


if __name__ == "__main__":
    unittest.main()
