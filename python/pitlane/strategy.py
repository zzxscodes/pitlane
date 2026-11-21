"""Python strategy side of the pitlane bridge.

The trading engine stays in C++. Python attaches to it through two wait-free
single-producer/single-consumer rings exposed by the C ABI: events flow from
the engine to the strategy, orders and queries flow back. A third bounded ring
carries strategy log lines to the engine. Records are 64-byte integers (128
bytes for a log line), so a strategy thread never touches a lock and a slow
handler only costs dropped records, which are counted.

Quick start::

    import pitlane

    class BuyTheDip(pitlane.Strategy):
        name = "dip"
        name_id = 7

        def on_quote(self, q):
            if q.px and q.px < q.a:            # last below the bid
                self.ctx.insert(self.name_id, 1, q.px, 1)

        def on_order(self, o):
            print("order", o.id, "status", o.status)

    bridge = pitlane.Bridge()
    run(BuyTheDip(bridge), bridge)

The engine calls :func:`Bridge.post` (``pl_bridge_post``) and drains commands
with :meth:`Bridge.take_cmd`; a single engine thread and a single strategy
thread own the bridge.
"""

from __future__ import annotations

import ctypes
import time

from . import _load

__all__ = [
    "ALGO_REQ",
    "ALGO_RSP",
    "ASSET",
    "BACKTEST_DONE",
    "Bridge",
    "CANCEL",
    "CANCEL_ALGO",
    "Cmd",
    "CONTROL_REQ",
    "CONTROL_RSP",
    "CONTROL_RSP_CMD",
    "Context",
    "Event",
    "INSERT",
    "INSERT_ALGO",
    "LOG",
    "LOG_DEBUG",
    "LOG_ERROR",
    "LOG_INFO",
    "LOG_TEXT",
    "LOG_TRACE",
    "LOG_WARN",
    "Log",
    "MKT_ORDER",
    "MKT_TRADE",
    "NEW_DATE",
    "NEW_PARAM",
    "ONE_DATE_DONE",
    "ONE_PARAM_DONE",
    "ORDER",
    "PACKAGE_REQ",
    "PACKAGE_RSP",
    "PAUSE",
    "POSITION",
    "PROPERTY",
    "QUERY_ASSET",
    "QUERY_POSITION",
    "QUERY_SOURCE",
    "QUOTE",
    "REBUILD_QUOTE",
    "RESUME",
    "SET_LOG_STATE",
    "SIGNAL",
    "SIGNAL_BATCH",
    "SIGNAL_SUB",
    "SIGNAL_UNSUB",
    "Strategy",
    "SUB_MKT_ORDER",
    "SUB_MKT_TRADE",
    "SUB_REBUILD",
    "SUBSCRIBE",
    "TIMER",
    "TRADE",
    "UNSUBSCRIBE",
    "now_us",
    "pump",
    "run",
]

QUOTE = 1
ORDER = 2
TRADE = 3
SIGNAL = 4
ASSET = 5
POSITION = 6
PACKAGE_REQ = 7
PACKAGE_RSP = 8
TIMER = 9
REBUILD_QUOTE = 10
MKT_ORDER = 11
MKT_TRADE = 12
ALGO_REQ = 13
ALGO_RSP = 14
SIGNAL_BATCH = 15
CONTROL_REQ = 16
CONTROL_RSP = 17
PROPERTY = 18
LOG = 19
NEW_PARAM = 20
ONE_PARAM_DONE = 21
NEW_DATE = 22
ONE_DATE_DONE = 23
BACKTEST_DONE = 24

INSERT = 1
CANCEL = 2
PAUSE = 3
RESUME = 4
SUBSCRIBE = 5
UNSUBSCRIBE = 6
QUERY_ASSET = 7
QUERY_POSITION = 8
SIGNAL_SUB = 9
SIGNAL_UNSUB = 10
INSERT_ALGO = 11
CANCEL_ALGO = 12
SUB_MKT_ORDER = 13
SUB_MKT_TRADE = 14
SUB_REBUILD = 15
QUERY_SOURCE = 16
SET_LOG_STATE = 17
CONTROL_RSP_CMD = 18

LOG_TRACE = 0
LOG_DEBUG = 1
LOG_INFO = 2
LOG_WARN = 3
LOG_ERROR = 4

LOG_TEXT = 112

_LOG_LEVELS = {
    LOG_TRACE: "trace",
    LOG_DEBUG: "debug",
    LOG_INFO: "info",
    LOG_WARN: "warn",
    LOG_ERROR: "error",
}

_EVENT_NAMES = {
    QUOTE: "quote",
    ORDER: "order",
    TRADE: "trade",
    SIGNAL: "signal",
    ASSET: "asset",
    POSITION: "position",
    PACKAGE_REQ: "package_req",
    PACKAGE_RSP: "package_rsp",
    TIMER: "timer",
    REBUILD_QUOTE: "rebuild_quote",
    MKT_ORDER: "mkt_order",
    MKT_TRADE: "mkt_trade",
    ALGO_REQ: "algo_req",
    ALGO_RSP: "algo_rsp",
    SIGNAL_BATCH: "signal_batch",
    CONTROL_REQ: "control_req",
    CONTROL_RSP: "control_rsp",
    PROPERTY: "property",
    LOG: "log",
    NEW_PARAM: "new_param",
    ONE_PARAM_DONE: "one_param_done",
    NEW_DATE: "new_date",
    ONE_DATE_DONE: "one_date_done",
    BACKTEST_DONE: "backtest_done",
}

_DEFAULT_BATCH = 256


class _PlEvent(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint64),
        ("stamp_us", ctypes.c_int64),
        ("px", ctypes.c_int64),
        ("qty", ctypes.c_int64),
        ("a", ctypes.c_int64),
        ("b", ctypes.c_int64),
        ("name", ctypes.c_uint32),
        ("seq", ctypes.c_uint16),
        ("kind", ctypes.c_uint8),
        ("status", ctypes.c_uint8),
        ("side", ctypes.c_uint8),
        ("pad", ctypes.c_uint8 * 7),
    ]


class _PlCmd(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint64),
        ("px", ctypes.c_int64),
        ("qty", ctypes.c_int64),
        ("a", ctypes.c_int64),
        ("b", ctypes.c_int64),
        ("name", ctypes.c_uint32),
        ("seq", ctypes.c_uint16),
        ("kind", ctypes.c_uint8),
        ("side", ctypes.c_uint8),
        ("status", ctypes.c_uint8),
        ("hedge", ctypes.c_uint8),
        ("pad", ctypes.c_uint8 * 14),
    ]


class _PlLog(ctypes.Structure):
    _fields_ = [
        ("stamp_us", ctypes.c_int64),
        ("seq", ctypes.c_uint32),
        ("level", ctypes.c_int32),
        ("text", ctypes.c_char * LOG_TEXT),
    ]


_EVENT_FIELDS = (
    "id",
    "stamp_us",
    "px",
    "qty",
    "a",
    "b",
    "name",
    "seq",
    "kind",
    "status",
    "side",
)


class Event:
    """One engine event, decoded from the 64-byte C record."""

    __slots__ = _EVENT_FIELDS

    def __init__(self, kind=0, **kwargs):
        self.kind = kind
        self.id = 0
        self.stamp_us = 0
        self.px = 0
        self.qty = 0
        self.a = 0
        self.b = 0
        self.name = 0
        self.seq = 0
        self.status = 0
        self.side = 0
        for key, value in kwargs.items():
            if key not in _EVENT_FIELDS:
                raise TypeError("unknown event field: %s" % key)
            setattr(self, key, value)

    @classmethod
    def _from(cls, raw):
        ev = cls.__new__(cls)
        for name in _EVENT_FIELDS:
            setattr(ev, name, getattr(raw, name))
        return ev

    @property
    def label(self):
        return _EVENT_NAMES.get(self.kind, "unknown")

    def as_tuple(self):
        return (self.kind,) + tuple(getattr(self, n) for n in _EVENT_FIELDS if n != "kind")

    def __repr__(self):
        body = ", ".join("%s=%d" % (n, getattr(self, n)) for n in _EVENT_FIELDS if n != "kind")
        return "Event(%s, %s)" % (self.label, body)


class Cmd:
    """One strategy command, as drained by the engine."""

    __slots__ = ("id", "px", "qty", "a", "b", "name", "seq", "kind", "side", "status", "hedge")

    def __init__(self, raw):
        for name in self.__slots__:
            setattr(self, name, getattr(raw, name))

    def __repr__(self):
        return "Cmd(kind=%d, id=%d, name=%d, px=%d, qty=%d, side=%d)" % (
            self.kind,
            self.id,
            self.name,
            self.px,
            self.qty,
            self.side,
        )


class Log:
    """One strategy-side log line drained by the engine."""

    __slots__ = ("stamp_us", "seq", "level", "text")

    def __init__(self, raw):
        self.stamp_us = raw.stamp_us
        self.seq = raw.seq
        self.level = raw.level
        self.text = raw.text.decode("utf-8", "replace")

    @property
    def label(self):
        return _LOG_LEVELS.get(self.level, "level%d" % self.level)

    def __repr__(self):
        return "Log(level=%s, seq=%d, text=%r)" % (self.label, self.seq, self.text)


def _bind(lib):
    if getattr(lib, "_pitlane_bridge_bound", False):
        return lib
    lib.pl_bridge_open.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    lib.pl_bridge_open.restype = ctypes.c_void_p
    lib.pl_bridge_close.argtypes = [ctypes.c_void_p]
    lib.pl_bridge_close.restype = None
    lib.pl_bridge_post.argtypes = [ctypes.c_void_p, ctypes.POINTER(_PlEvent)]
    lib.pl_bridge_post.restype = ctypes.c_int
    lib.pl_bridge_poll.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_PlEvent),
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    lib.pl_bridge_poll.restype = ctypes.c_int
    lib.pl_bridge_send.argtypes = [ctypes.c_void_p, ctypes.POINTER(_PlCmd)]
    lib.pl_bridge_send.restype = ctypes.c_int
    lib.pl_bridge_take_cmd.argtypes = [ctypes.c_void_p, ctypes.POINTER(_PlCmd)]
    lib.pl_bridge_take_cmd.restype = ctypes.c_int
    lib.pl_bridge_events_pending.argtypes = [ctypes.c_void_p]
    lib.pl_bridge_events_pending.restype = ctypes.c_uint32
    lib.pl_bridge_cmds_pending.argtypes = [ctypes.c_void_p]
    lib.pl_bridge_cmds_pending.restype = ctypes.c_uint32
    lib.pl_bridge_events_lost.argtypes = [ctypes.c_void_p]
    lib.pl_bridge_events_lost.restype = ctypes.c_uint64
    lib.pl_bridge_cmds_lost.argtypes = [ctypes.c_void_p]
    lib.pl_bridge_cmds_lost.restype = ctypes.c_uint64
    lib.pl_bridge_log.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_char_p]
    lib.pl_bridge_log.restype = ctypes.c_int
    lib.pl_bridge_take_log.argtypes = [ctypes.c_void_p, ctypes.POINTER(_PlLog)]
    lib.pl_bridge_take_log.restype = ctypes.c_int
    lib.pl_bridge_logs_pending.argtypes = [ctypes.c_void_p]
    lib.pl_bridge_logs_pending.restype = ctypes.c_uint32
    lib.pl_bridge_logs_lost.argtypes = [ctypes.c_void_p]
    lib.pl_bridge_logs_lost.restype = ctypes.c_uint64
    lib.pl_now_us.argtypes = []
    lib.pl_now_us.restype = ctypes.c_int64
    lib._pitlane_bridge_bound = True
    return lib


def now_us():
    """Monotonic engine-grade clock in microseconds."""
    return _bind(_load()).pl_now_us()


def _event_raw(kind, kwargs):
    raw = _PlEvent()
    raw.kind = kind
    for key, value in kwargs.items():
        if key not in _EVENT_FIELDS or key == "kind":
            raise TypeError("unknown event field: %s" % key)
        setattr(raw, key, int(value))
    return raw


def _cmd_raw(kind, kwargs):
    raw = _PlCmd()
    raw.kind = kind
    for key, value in kwargs.items():
        if key in ("kind",) or not hasattr(raw, key):
            raise TypeError("unknown command field: %s" % key)
        setattr(raw, key, int(value))
    return raw


class Bridge:
    """Fixed rings shared with one engine thread."""

    def __init__(self, event_cap=0, cmd_cap=0):
        self._lib = _bind(_load())
        self._h = self._lib.pl_bridge_open(event_cap, cmd_cap)
        if not self._h:
            raise MemoryError("cannot allocate the strategy bridge")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def close(self):
        if self._h:
            self._lib.pl_bridge_close(self._h)
            self._h = None

    def _handle(self):
        if not self._h:
            raise RuntimeError("bridge is closed")
        return self._h

    def post(self, kind, **kwargs):
        """Engine side: enqueue an event; False when the ring is full."""
        return bool(self._lib.pl_bridge_post(self._handle(), ctypes.byref(_event_raw(kind, kwargs))))

    def poll(self, max_events=_DEFAULT_BATCH):
        """Strategy side: drain up to ``max_events`` events in order."""
        if max_events <= 0:
            return []
        buf = (_PlEvent * max_events)()
        n = ctypes.c_uint32(0)
        if not self._lib.pl_bridge_poll(self._handle(), buf, max_events, ctypes.byref(n)):
            raise RuntimeError("bridge poll failed")
        return [Event._from(buf[i]) for i in range(n.value)]

    def send(self, kind, **kwargs):
        """Strategy side: enqueue an order or query; False when full."""
        return bool(self._lib.pl_bridge_send(self._handle(), ctypes.byref(_cmd_raw(kind, kwargs))))

    def take_cmd(self):
        """Engine side: take one command, or None when idle."""
        raw = _PlCmd()
        if not self._lib.pl_bridge_take_cmd(self._handle(), ctypes.byref(raw)):
            return None
        return Cmd(raw)

    def log(self, level, text):
        """Strategy side: enqueue one log line; False when the log ring is full."""
        raw = text if isinstance(text, bytes) else str(text).encode("utf-8")
        return bool(self._lib.pl_bridge_log(self._handle(), int(level), raw))

    def take_log(self):
        """Engine side: take one log line, or None when idle."""
        raw = _PlLog()
        if not self._lib.pl_bridge_take_log(self._handle(), ctypes.byref(raw)):
            return None
        return Log(raw)

    @property
    def events_pending(self):
        return self._lib.pl_bridge_events_pending(self._handle())

    @property
    def cmds_pending(self):
        return self._lib.pl_bridge_cmds_pending(self._handle())

    @property
    def events_lost(self):
        return self._lib.pl_bridge_events_lost(self._handle())

    @property
    def cmds_lost(self):
        return self._lib.pl_bridge_cmds_lost(self._handle())

    @property
    def logs_pending(self):
        return self._lib.pl_bridge_logs_pending(self._handle())

    @property
    def logs_lost(self):
        return self._lib.pl_bridge_logs_lost(self._handle())


class Context:
    """Order and query entry points handed to a strategy."""

    __slots__ = ("bridge", "strategy")

    def __init__(self, bridge, strategy=""):
        self.bridge = bridge
        self.strategy = strategy

    def insert(self, name, side, px, qty, id=0, seq=0, a=0, b=0, hedge=0):
        """Submit a limit order; ``hedge`` is 0 speculation, 1 arbitrage, 2 hedge."""
        return self.bridge.send(
            INSERT, name=name, side=side, px=px, qty=qty, id=id, seq=seq, a=a, b=b,
            hedge=hedge,
        )

    def cancel(self, id, name=0, seq=0):
        return self.bridge.send(CANCEL, id=id, name=name, seq=seq)

    def pause(self, name=0):
        return self.bridge.send(PAUSE, name=name)

    def resume(self, name=0):
        return self.bridge.send(RESUME, name=name)

    def subscribe(self, name):
        return self.bridge.send(SUBSCRIBE, name=name)

    def unsubscribe(self, name):
        return self.bridge.send(UNSUBSCRIBE, name=name)

    def query_asset(self, token=0):
        return self.bridge.send(QUERY_ASSET, id=token)

    def query_position(self, name=0, token=0):
        return self.bridge.send(QUERY_POSITION, id=token, name=name)

    def signal_sub(self, name=0, key=0):
        """Subscribe to one named signal; ``key`` is the engine-side key."""
        return self.bridge.send(SIGNAL_SUB, name=name, id=key)

    def signal_unsub(self, name=0, key=0):
        return self.bridge.send(SIGNAL_UNSUB, name=name, id=key)

    def insert_algo(self, name, side, px, qty, id=0, seq=0, a=0, b=0):
        """Submit an algo order; ``a`` and ``b`` carry compact algo parameters."""
        return self.bridge.send(
            INSERT_ALGO, name=name, side=side, px=px, qty=qty, id=id, seq=seq, a=a, b=b
        )

    def cancel_algo(self, id, name=0, seq=0):
        return self.bridge.send(CANCEL_ALGO, id=id, name=name, seq=seq)

    def _subscribe_many(self, kind, names):
        if isinstance(names, int):
            return self.bridge.send(kind, name=names)
        ok = True
        for name in names:
            ok = self.bridge.send(kind, name=int(name)) and ok
        return ok

    def subscribe_mkt_order(self, names):
        """Subscribe to order-by-order market data for one name or an iterable."""
        return self._subscribe_many(SUB_MKT_ORDER, names)

    def subscribe_mkt_trade(self, names):
        """Subscribe to market trades for one name or an iterable."""
        return self._subscribe_many(SUB_MKT_TRADE, names)

    def subscribe_rebuild(self, name):
        """Subscribe to order-book rebuilds for one instrument."""
        return self.bridge.send(SUB_REBUILD, name=name)

    def query_source(self, token=0, source=0):
        """Query assets or positions from a chosen source id."""
        return self.bridge.send(QUERY_SOURCE, id=token, a=source)

    def set_log_state(self, level=0):
        return self.bridge.send(SET_LOG_STATE, status=level)

    def control_rsp(self, id, status=0, name=0):
        return self.bridge.send(CONTROL_RSP_CMD, id=id, name=name, status=status)

    def log(self, level, text):
        """Send one log line to the engine log."""
        return self.bridge.log(level, text)


class Strategy:
    """Subclass and override the handlers you need.

    ``name`` is the strategy tag reported to the engine; ``bridge`` is optional
    so a strategy can be unit tested by calling :meth:`on_event` directly.
    """

    name = ""
    kind = 0

    def __init__(self, bridge=None):
        self.bridge = bridge
        self.ctx = Context(bridge, self.name) if bridge is not None else None
        self.stopped = False
        self.errors = 0

    def stop(self):
        self.stopped = True

    def log(self, level, text):
        """Send one log line to the engine log; a detached strategy drops it."""
        return self.bridge.log(level, text) if self.bridge is not None else False

    def debug(self, text):
        return self.log(LOG_DEBUG, text)

    def info(self, text):
        return self.log(LOG_INFO, text)

    def warn(self, text):
        return self.log(LOG_WARN, text)

    def error(self, text):
        return self.log(LOG_ERROR, text)

    def on_start(self):
        pass

    def on_stop(self):
        pass

    def on_quote(self, ev):
        pass

    def on_order(self, ev):
        pass

    def on_trade(self, ev):
        pass

    def on_signal(self, ev):
        pass

    def on_asset(self, ev):
        pass

    def on_position(self, ev):
        pass

    def on_package_req(self, ev):
        pass

    def on_package_rsp(self, ev):
        pass

    def on_timer(self, ev):
        pass

    def on_rebuild_quote(self, ev):
        pass

    def on_mkt_order(self, ev):
        pass

    def on_mkt_trade(self, ev):
        pass

    def on_algo_req(self, ev):
        pass

    def on_algo_rsp(self, ev):
        pass

    def on_signal_batch(self, ev):
        pass

    def on_control_req(self, ev):
        pass

    def on_control_rsp(self, ev):
        pass

    def on_property(self, ev):
        pass

    def on_log(self, ev):
        pass

    def on_new_param(self, ev):
        pass

    def on_one_param_done(self, ev):
        pass

    def on_new_date(self, ev):
        pass

    def on_one_date_done(self, ev):
        pass

    def on_backtest_done(self, ev):
        pass

    def on_event(self, ev):
        """Dispatch one event; unknown kinds are ignored."""
        name = _HANDLERS.get(ev.kind)
        if name is not None:
            getattr(self, name)(ev)


_HANDLERS = {
    QUOTE: "on_quote",
    ORDER: "on_order",
    TRADE: "on_trade",
    SIGNAL: "on_signal",
    ASSET: "on_asset",
    POSITION: "on_position",
    PACKAGE_REQ: "on_package_req",
    PACKAGE_RSP: "on_package_rsp",
    TIMER: "on_timer",
    REBUILD_QUOTE: "on_rebuild_quote",
    MKT_ORDER: "on_mkt_order",
    MKT_TRADE: "on_mkt_trade",
    ALGO_REQ: "on_algo_req",
    ALGO_RSP: "on_algo_rsp",
    SIGNAL_BATCH: "on_signal_batch",
    CONTROL_REQ: "on_control_req",
    CONTROL_RSP: "on_control_rsp",
    PROPERTY: "on_property",
    LOG: "on_log",
    NEW_PARAM: "on_new_param",
    ONE_PARAM_DONE: "on_one_param_done",
    NEW_DATE: "on_new_date",
    ONE_DATE_DONE: "on_one_date_done",
    BACKTEST_DONE: "on_backtest_done",
}


def pump(strategy, bridge=None, max_events=_DEFAULT_BATCH):
    """Drain and dispatch one batch; returns how many events were handled."""
    bridge = bridge or strategy.bridge
    if bridge is None:
        raise ValueError("strategy has no bridge")
    events = bridge.poll(max_events)
    for ev in events:
        strategy.on_event(ev)
    return len(events)


def run(strategy, bridge=None, idle_sleep=0.0002, max_events=_DEFAULT_BATCH):
    """Serve until :meth:`Strategy.stop` (or Ctrl-C); returns handled count.

    ``idle_sleep`` yields the CPU when the ring is empty; pass 0 for a busy
    loop when the engine and strategy share a thread.
    """
    bridge = bridge or strategy.bridge
    if bridge is None:
        raise ValueError("strategy has no bridge")
    handled = 0
    strategy.on_start()
    try:
        while not strategy.stopped:
            count = pump(strategy, bridge, max_events)
            handled += count
            if count == 0 and idle_sleep:
                time.sleep(idle_sleep)
    except KeyboardInterrupt:
        pass
    finally:
        strategy.on_stop()
    return handled
