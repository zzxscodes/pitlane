# pitlane_simnow

Self-contained SimNow 7×24 example: MD → strategy → real IOC → TD accept, with local/gate/roundtrip T2T.

## Layout

```
examples/simnow/
  main.cpp                 # closed-loop host
  ctp/                     # CTP gateway on MdEngine and TdClient
  third_party/ctp/         # vendored official CTP API (_se headers + Linux libs)
  conf/paper.conf          # center loop, no broker
  conf/simnow_7x24.conf.example
  conf/simnow_7x24.conf    # local secrets (gitignored)
  run/                     # flow_*, desk.bin, wire.log (gitignored contents)
  README.md
```

Account, engines, and `WireLog` are pitlane framework types. `ctp/` is this example's gateway: it subclasses the library and talks to CTP. The library does not ship that code. `mode = paper` runs the trading center (`Center` + `TapeMd` + `PaperVenue`) with no broker. `mode = live` uses the same desk with this gateway. Progress is written to `run/wire.log`.

```bash
../../build/pitlane_simnow conf/paper.conf
```

`pitlane_simnow_bench` times that paper center without a broker library. It also checks the scale, lap, and histogram helpers, and times one call inside the ring, the order gate, and the paper venue. `ctest` runs it as `simnow_bench`.

```bash
cmake --build build -j --target pitlane_simnow_bench
./build/pitlane_simnow_bench
```

## Build

The official CTP `_se` headers and Linux shared objects are vendored under
`third_party/ctp` (official package `v6.7.13_20260225`, from the SimNow / SFIT
API download; the archive copy came from the public `pseudocodes/ctp2rs` mirror
because the SimNow download page is behind a WAF on some networks). Point
`PITLANE_CTP_API_ROOT` at another unpacked `_se` package to override. From the
repo root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPITLANE_BUILD_CTP=ON
cmake --build build -j --target pitlane_simnow
```

## Run

```bash
cd examples/simnow
cp -n conf/simnow_7x24.conf.example conf/simnow_7x24.conf   # edit password
../../build/pitlane_simnow conf/simnow_7x24.conf
```

Optional: `SIMNOW_PASSWORD`, `SIMNOW_INVESTOR`, `SIMNOW_WORK_DIR`.

Runtime files only under `./run` (or `work_dir` in conf).

## Full-flow validation

`--full` and `--backtest` run one instance of every core pitlane path in a
single process:

```
tape quotes -> FactorPipeline (Momentum / OFI / ZScore) -> signal
  -> RMS pre-trade check -> EMS parent slice -> SOR venue choice
  -> OMS state machine -> PaperVenue (paper/backtest) or real CTP IOC (live)
  -> PMS position/PnL -> WAL append + recover -> snapshot publish -> tail latency
```

Only the first subscribed instrument is used as the factor series; other
instruments still connect and receive quotes.

Three stages of the same code path:

```bash
cd examples/simnow

# 1. Strategy development / backtest: replay a recorded tape, no broker.
#    The tape is replayed to the end so every parent gets a chance to complete.
../../build/pitlane_simnow --backtest --tape tape.csv conf/paper.conf

# 2. Paper trading: same desk, synthetic tape, stops as soon as the flow passes.
../../build/pitlane_simnow --full conf/paper.conf

# 3. Live: real SimNow market data and orders.
../../build/pitlane_simnow --full conf/simnow_7x24.conf
```

`--backtest` without `--tape` falls back to the built-in synthetic tape. The
CSV tape has one quote per row, header optional, no spaces required:

```
exch_us,name,bid_px,ask_px,last_px,bid_qty,ask_qty
1710000000000000,1,999950,1000050,1000000,5,5
```

Prices are integer ticks as used by `pitlane::Quote` (`px_x10000`). `exch_us`
and `local_us` are both set to the timestamp column. Lines that do not parse
as seven integers are skipped; `#` starts a comment.

Paper matching timing comes from the `[venue]` section of the conf
(`go_us0`, `pull_us0`, `tape_first`); `conf_load` fills `VenueCfg` and both
`run_paper` and `run_sim_flow` use it.

A run is `PASS` when at least one parent completes, at least one child fills,
the WAL replay count matches what was appended, the snapshot latest sequence
matches the published sequence, and both OMS and PMS hold rows. The summary
prints OMS/EMS/PMS counters, factor values and evaluation, WAL/snapshot state,
and quote/report tail percentiles.

Files created under `run/`:

- `flow.wal` — framed append log of parent marks, intents, acks, and fills.
- `flow.snap` — cross-process read-only snapshot ring of flow state.
- `wire.log` — same progress log as the T2T mode.

Live failures are reported with the CTP error id in `run/wire.log`. Common
cases: wrong/expired password (`141`), missing authentication (`3`), contract
not trading in this session, or settlement not confirmed (`7`). The example
does not change the account password automatically.

