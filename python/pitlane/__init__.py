"""ctypes bindings for the optional pitlane C ABI (``libpitlane_c.so``).

Two surfaces are exposed:

* :mod:`pitlane` cold-path research: columnar block tapes, row ranges, and a
  single-instrument backtest.
* :class:`pitlane.Bridge` and :class:`pitlane.Strategy`: a wait-free
  event/command bridge that lets a Python strategy thread attach to the C++
  engine. The engine hot path never calls into Python; a late strategy loses
  records (counted) instead of adding latency.

Quick start::

    import pitlane

    with pitlane.Tape.open("run/tape.plt") as tape:
        print(tape.meta())
        ticks = tape.rows(limit=10)
        print(ticks[0].as_tuple())

    print(pitlane.backtest("run/tape.plt", kind=2, window=16, threshold_ppm=10))

    class Dip(pitlane.Strategy):
        def on_quote(self, q):
            if q.px and q.px < q.a:
                self.ctx.insert(7, 1, q.px, 1)

    bridge = pitlane.Bridge()
    pitlane.run(Dip(bridge), bridge)

The library is looked up in this order:

1. ``$PITLANE_CAPI_LIB``
2. ``libpitlane_c.so`` next to this package
3. ``<repo>/build/libpitlane_c.so`` (``../../build`` from this file)
4. ``ctypes.util.find_library("pitlane_c")``

Only the standard library is used; Python 3.8 or newer.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os

__all__ = [
    "ALGO_REQ",
    "ALGO_RSP",
    "ASSET",
    "BACKTEST_DONE",
    "Bridge",
    "CANCEL",
    "CANCEL_ALGO",
    "CONTROL_REQ",
    "CONTROL_RSP",
    "CONTROL_RSP_CMD",
    "Cmd",
    "Context",
    "Event",
    "INSERT",
    "INSERT_ALGO",
    "INT64_MIN",
    "INT64_MAX",
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
    "SUBSCRIBE",
    "SUB_MKT_ORDER",
    "SUB_MKT_TRADE",
    "SUB_REBUILD",
    "Strategy",
    "Tape",
    "Tick",
    "TIMER",
    "TRADE",
    "UNSUBSCRIBE",
    "backtest",
    "now_us",
    "pump",
    "run",
    "strategy",
    "version",
    "write_tape",
]

INT64_MIN = -(2**63)
INT64_MAX = 2**63 - 1

_COLUMN_NAMES = (
    "exch_us",
    "local_us",
    "bid_px",
    "ask_px",
    "last_px",
    "bid_qty",
    "ask_qty",
    "last_qty",
)

_PAGE = 500


class _PlTick(ctypes.Structure):
    _fields_ = [
        ("exch_us", ctypes.c_int64),
        ("local_us", ctypes.c_int64),
        ("bid_px", ctypes.c_int64),
        ("ask_px", ctypes.c_int64),
        ("last_px", ctypes.c_int64),
        ("bid_qty", ctypes.c_int64),
        ("ask_qty", ctypes.c_int64),
        ("last_qty", ctypes.c_int64),
    ]


class _PlMeta(ctypes.Structure):
    _fields_ = [
        ("versions", ctypes.c_uint32),
        ("blocks", ctypes.c_uint32),
        ("symbol", ctypes.c_uint32),
        ("rows", ctypes.c_uint64),
        ("bytes", ctypes.c_uint64),
        ("first_ts", ctypes.c_int64),
        ("last_ts", ctypes.c_int64),
    ]


class _PlStats(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("rows", ctypes.c_uint64),
        ("min", ctypes.c_int64 * 8),
        ("max", ctypes.c_int64 * 8),
        ("last", ctypes.c_int64 * 8),
        ("mean", ctypes.c_double * 8),
    ]


class _PlQuery(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("since_us", ctypes.c_int64),
        ("until_us", ctypes.c_int64),
        ("offset", ctypes.c_uint32),
        ("limit", ctypes.c_uint32),
        ("cols", ctypes.c_uint8),
        ("pad", ctypes.c_uint8 * 3),
    ]


class _PlBtCfg(ctypes.Structure):
    _fields_ = [
        ("cash", ctypes.c_int64),
        ("qty", ctypes.c_int64),
        ("fee_ppm", ctypes.c_int64),
        ("min_fee", ctypes.c_int64),
        ("slip_ticks", ctypes.c_int64),
        ("tick", ctypes.c_int64),
        ("threshold_ppm", ctypes.c_int64),
        ("cooldown_us", ctypes.c_int64),
        ("sign", ctypes.c_int8),
        ("kind", ctypes.c_uint8),
        ("window", ctypes.c_uint16),
        ("stride", ctypes.c_int32),
    ]


class _PlBtMetrics(ctypes.Structure):
    _fields_ = [
        ("points", ctypes.c_int64),
        ("trades", ctypes.c_int64),
        ("round_trips", ctypes.c_int64),
        ("wins", ctypes.c_int64),
        ("gross_pnl", ctypes.c_int64),
        ("fees", ctypes.c_int64),
        ("net_pnl", ctypes.c_int64),
        ("turnover", ctypes.c_int64),
        ("return_ppm", ctypes.c_int64),
        ("annual_ppm", ctypes.c_int64),
        ("sharpe_ppm", ctypes.c_int64),
        ("max_dd_ppm", ctypes.c_int64),
        ("win_ppm", ctypes.c_int64),
        ("exposure_ppm", ctypes.c_int64),
        ("avg_trade_pnl", ctypes.c_int64),
        ("span_us", ctypes.c_int64),
        ("final_equity", ctypes.c_int64),
        ("peak_equity", ctypes.c_int64),
    ]


_LIB = None


def _search_paths():
    here = os.path.dirname(os.path.abspath(__file__))
    paths = []
    env = os.environ.get("PITLANE_CAPI_LIB")
    if env:
        paths.append(env)
    paths.append(os.path.join(here, "libpitlane_c.so"))
    paths.append(os.path.normpath(os.path.join(here, "..", "..", "build", "libpitlane_c.so")))
    return paths


def _load():
    global _LIB
    if _LIB is not None:
        return _LIB
    tried = _search_paths()
    for path in tried:
        if path and os.path.isfile(path):
            _LIB = ctypes.CDLL(path)
            break
    else:
        found = ctypes.util.find_library("pitlane_c")
        if found:
            _LIB = ctypes.CDLL(found)
        else:
            raise RuntimeError(
                "libpitlane_c.so not found; build the pitlane_c target or set "
                "PITLANE_CAPI_LIB (tried: %s)" % ", ".join(tried)
            )

    lib = _LIB
    lib.pl_tape_open.argtypes = [ctypes.c_char_p]
    lib.pl_tape_open.restype = ctypes.c_void_p
    lib.pl_tape_close.argtypes = [ctypes.c_void_p]
    lib.pl_tape_close.restype = None
    lib.pl_tape_meta.argtypes = [ctypes.c_void_p, ctypes.POINTER(_PlMeta)]
    lib.pl_tape_meta.restype = ctypes.c_int
    lib.pl_tape_rows.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_PlQuery),
        ctypes.POINTER(_PlTick),
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    lib.pl_tape_rows.restype = ctypes.c_int
    lib.pl_tape_describe.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(_PlStats)]
    lib.pl_tape_describe.restype = ctypes.c_int
    lib.pl_tape_write.argtypes = [
        ctypes.c_char_p,
        ctypes.c_uint32,
        ctypes.POINTER(_PlTick),
        ctypes.c_uint32,
    ]
    lib.pl_tape_write.restype = ctypes.c_int
    lib.pl_backtest.argtypes = [ctypes.c_char_p, ctypes.POINTER(_PlBtCfg), ctypes.POINTER(_PlBtMetrics)]
    lib.pl_backtest.restype = ctypes.c_int
    lib.pl_version.argtypes = []
    lib.pl_version.restype = ctypes.c_char_p
    return _LIB


def _path_bytes(path):
    return os.fsencode(os.fspath(path))


class Tick:
    """One tape row, in the same integer units as the C struct."""

    __slots__ = _COLUMN_NAMES

    def __init__(
        self,
        exch_us=0,
        local_us=0,
        bid_px=0,
        ask_px=0,
        last_px=0,
        bid_qty=0,
        ask_qty=0,
        last_qty=0,
    ):
        self.exch_us = exch_us
        self.local_us = local_us
        self.bid_px = bid_px
        self.ask_px = ask_px
        self.last_px = last_px
        self.bid_qty = bid_qty
        self.ask_qty = ask_qty
        self.last_qty = last_qty

    def as_tuple(self):
        return tuple(getattr(self, name) for name in _COLUMN_NAMES)

    def __repr__(self):
        return "Tick(%s)" % ", ".join("%s=%d" % (n, getattr(self, n)) for n in _COLUMN_NAMES)


def _tick_from(pl):
    return Tick(
        pl.exch_us,
        pl.local_us,
        pl.bid_px,
        pl.ask_px,
        pl.last_px,
        pl.bid_qty,
        pl.ask_qty,
        pl.last_qty,
    )


class Tape:
    """A read handle to one block tape file; use :meth:`open` or ``with``."""

    def __init__(self, handle, path):
        self._h = handle
        self._path = path

    @classmethod
    def open(cls, path):
        lib = _load()
        handle = lib.pl_tape_open(_path_bytes(path))
        if not handle:
            raise OSError("cannot open block tape: %s" % os.fspath(path))
        return cls(handle, path)

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
            _load().pl_tape_close(self._h)
            self._h = None

    def _handle(self):
        if not self._h:
            raise RuntimeError("tape is closed")
        return self._h

    def meta(self):
        """Header summary: versions, blocks, symbol, rows, bytes, first/last ts."""
        lib = _load()
        out = _PlMeta()
        if not lib.pl_tape_meta(self._handle(), ctypes.byref(out)):
            raise RuntimeError("tape meta failed: %s" % os.fspath(self._path))
        return {
            "versions": out.versions,
            "blocks": out.blocks,
            "symbol": out.symbol,
            "rows": out.rows,
            "bytes": out.bytes,
            "first_ts": out.first_ts,
            "last_ts": out.last_ts,
        }

    def rows(self, version=0, since_us=INT64_MIN, until_us=INT64_MAX, offset=0, limit=100, cols=0xFF):
        """Rows of ``version`` (0 = latest) in pages of 500.

        ``since_us`` and ``until_us`` are inclusive; ``offset`` and ``limit``
        apply after the time filter and ``limit`` 0 means no limit. ``cols`` is
        a bitmask in on-disk order; bits that are off stay 0 in the returned
        :class:`Tick`.
        """
        lib = _load()
        handle = self._handle()
        if offset < 0:
            raise ValueError("offset must be >= 0")
        if limit < 0:
            raise ValueError("limit must be >= 0")
        out = []
        while True:
            want = _PAGE
            if limit:
                remaining = limit - len(out)
                if remaining <= 0:
                    break
                want = min(_PAGE, remaining)
            buf = (_PlTick * want)()
            query = _PlQuery()
            query.version = version
            query.since_us = since_us
            query.until_us = until_us
            query.offset = offset + len(out)
            query.limit = want
            query.cols = cols & 0xFF
            n = ctypes.c_uint32(0)
            if not lib.pl_tape_rows(handle, ctypes.byref(query), buf, want, ctypes.byref(n)):
                raise RuntimeError(
                    "tape rows failed (version %d): %s" % (version, os.fspath(self._path))
                )
            if n.value == 0:
                break
            out.extend(_tick_from(buf[i]) for i in range(n.value))
            if n.value < want:
                break
        return out[:limit] if limit else out

    def describe(self, version=0):
        """Per-column min, max, last, mean, and row count for ``version``."""
        lib = _load()
        stats = _PlStats()
        if not lib.pl_tape_describe(self._handle(), version, ctypes.byref(stats)):
            raise RuntimeError(
                "tape describe failed (version %d): %s" % (version, os.fspath(self._path))
            )
        return [
            {
                "name": _COLUMN_NAMES[i],
                "min": stats.min[i],
                "max": stats.max[i],
                "last": stats.last[i],
                "mean": stats.mean[i],
                "rows": stats.rows,
            }
            for i in range(8)
        ]


def write_tape(path, ticks, symbol=0):
    """Write ``ticks`` (8-tuples or :class:`Tick`) as one committed version.

    The input is staged in batches of 4096 rows and committed once, so the file
    ends up with a single version regardless of its length.
    """
    lib = _load()
    staged = []
    batch = []
    for tick in ticks:
        if isinstance(tick, Tick):
            batch.append(_PlTick(*tick.as_tuple()))
        else:
            values = tuple(tick)
            if len(values) != 8:
                raise TypeError("each tick must be a Tick or an 8-tuple")
            batch.append(_PlTick(*values))
        if len(batch) >= 4096:
            staged.extend(batch)
            batch = []
    staged.extend(batch)
    n = len(staged)
    buf = (_PlTick * n)(*staged) if n else _PlTick()
    if not lib.pl_tape_write(_path_bytes(path), symbol, buf, n):
        raise OSError("cannot write block tape: %s" % os.fspath(path))


_BT_DEFAULTS = {
    "cash": 1000000,
    "qty": 1,
    "fee_ppm": 20,
    "min_fee": 0,
    "slip_ticks": 0,
    "tick": 0,
    "threshold_ppm": 10,
    "cooldown_us": 0,
    "sign": 1,
    "kind": 2,
    "window": 16,
    "stride": 1,
}


def backtest(path, **kwargs):
    """Run the single-instrument backtester and return every metric as a dict."""
    unknown = sorted(set(kwargs) - set(_BT_DEFAULTS))
    if unknown:
        raise TypeError("unknown backtest option: %s" % ", ".join(unknown))
    values = dict(_BT_DEFAULTS)
    values.update(kwargs)
    cfg = _PlBtCfg(**values)
    out = _PlBtMetrics()
    if not _load().pl_backtest(_path_bytes(path), ctypes.byref(cfg), ctypes.byref(out)):
        raise OSError("backtest failed: %s" % os.fspath(path))
    return {name: getattr(out, name) for name, _ in _PlBtMetrics._fields_}


def version():
    """Library version string, for example ``"1.0.0"``."""
    raw = _load().pl_version()
    return raw.decode("utf-8", "replace") if raw else ""


from . import strategy as strategy  # noqa: E402
from .strategy import (  # noqa: E402
    ALGO_REQ,
    ALGO_RSP,
    ASSET,
    BACKTEST_DONE,
    CANCEL,
    CANCEL_ALGO,
    CONTROL_REQ,
    CONTROL_RSP,
    CONTROL_RSP_CMD,
    INSERT,
    INSERT_ALGO,
    LOG,
    LOG_DEBUG,
    LOG_ERROR,
    LOG_INFO,
    LOG_TEXT,
    LOG_TRACE,
    LOG_WARN,
    Log,
    MKT_ORDER,
    MKT_TRADE,
    NEW_DATE,
    NEW_PARAM,
    ONE_DATE_DONE,
    ONE_PARAM_DONE,
    ORDER,
    PACKAGE_REQ,
    PACKAGE_RSP,
    PAUSE,
    POSITION,
    PROPERTY,
    QUERY_ASSET,
    QUERY_POSITION,
    QUERY_SOURCE,
    QUOTE,
    REBUILD_QUOTE,
    RESUME,
    SET_LOG_STATE,
    SIGNAL,
    SIGNAL_BATCH,
    SIGNAL_SUB,
    SIGNAL_UNSUB,
    SUBSCRIBE,
    SUB_MKT_ORDER,
    SUB_MKT_TRADE,
    SUB_REBUILD,
    TIMER,
    TRADE,
    UNSUBSCRIBE,
    Bridge,
    Cmd,
    Context,
    Event,
    Strategy,
    now_us,
    pump,
    run,
)
