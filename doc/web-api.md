# pitlane web API

The console in `client/index.html` and any other client talk to the hub over
HTTP/1.1 JSON on the port given by `--web` (default `8080`, loopback only unless
`--web-bind` is set). There is no separate admin socket.

Everything sits under `/api/`. Requests and replies are `application/json`
except `/api/health` (plain text) and `/api/login`, which may also be sent as a
form.

## Sessions and modes

`POST /api/login` takes `{"user":"admin","password":"...","mode":"test"}` or
`mode:"live"` and returns `{"code":0,"token":"...","role":"...","perms":N,
"mode":"test","user":"..."}`. Send the token as `Authorization: Bearer <token>`
on every other call; the console also stores it in a cookie. `POST /api/logout`
drops it.

A session is bound to the mode it logged in with. Gateway, account, alarm,
position, and clearing lists are filtered by that mode, and a request that
names an account or gateway of the other mode fails with `409` instead of
falling back. `GET /api/me` reports `user`, `role`, `perms`, and `mode`.

Permission bits decide which calls succeed: a call without the needed bit
answers `403`. `viewer` may read market, risk, and ops views; `trader` adds
tickets; `risk` adds control, alarms, and clearing; `ops` adds gateway control;
`admin` adds user management.

Errors are JSON `{"code":N,"msg":"..."}` with an HTTP status of `400`, `401`,
`403`, `404`, or `409`. Ticket and control replies also carry `code` and `msg`
with HTTP `200` when the request itself was well formed — check both. A `POST`
may carry no body; actions that only need the query string work that way, and
query values are percent-decoded.

## Market

| Call | Permission | Description |
| --- | --- | --- |
| `GET /api/symbols` | market | Products that currently have a series |
| `GET /api/instruments` | market | `{instruments:[{name,klass,ccy,tick,mult,margin_ppm,fee_ppm,min_fee,stamp_ppm,short,gw}]}` |
| `GET /api/accounts` | market | `{accounts:[{num,id,name,class,ccy,cash,env,gateways:[...],td_running}]}`; only accounts of the current session mode |
| `GET /api/series?name=1&tf_ms=60000&src=live&limit=240` | market | `{bars:[{ts,open,high,low,close,volume}],last:{...}}`; `src=tape` reads the recorded tape |
| `GET /api/ticks?name=1&limit=300` | market | Recent ticks from the live ring |
| `GET /api/history?name=1&since_us=0&until_us=0&limit=300` | market | Ticks read back from the tape file |

`name` is the numeric product id from `run/instruments.conf`.

## CTP depth, prints and latency

The CTP market-data adapter decodes each depth frame, maintains the book
incrementally, rebuilds prints and fans them out; the console reads the same live
view. Without CTP (paper mode) `available` is 0 and the counters stay at 0.

| Call | Permission | Description |
| --- | --- | --- |
| `GET /api/market/book` | market | `{available,decoded,decode_err,prints,books,stale,crossed,rebuilds,fan_sinks,book:[{name,status,event,levels,exch_us,last,upper,lower,imbalance_ppm,snaps,updates,rebuilds,stale,crossed,bid:[[px,qty]x5],ask:[[px,qty]x5]}],ticks:[{name,aggressor,exch_us,px,qty,volume}]}`; four most recently updated books and the last twelve prints |
| `GET /api/market/prints` | market | Same payload, named for print consumers |
| `GET /api/perf/hops` | market | `{scale_on,md:{stages:[{stage,n,min_ns,p50_ns,p99_ns,max_ns,over}],total},ops:{...}}`; `md` is the market-data thread (wire in → decode → book → fanout), `ops` is the console beat loop |
| `GET /api/perf/baseline?tolerance_ppm=200000` | market | Per-line comparison `{rev,base_rows,cur_rows,regressed,diffs:[{name,base_p99,cur_p99,delta_ppm,regressed,missing}],current}` |
| `POST /api/perf/baseline` | gateway | Writes the current profile to `<conf dir>/perf-baseline.txt`, returns `{code,rows,path}` |
| `GET /api/perf/flame` | market | `{nodes,depth,folded}`; folded stacks feed `flamegraph.pl` directly |
| `POST /api/perf/flame` | ops view | Clears the accumulated flame data |

Percentiles come from magnitude histograms and return the bucket lower edge. `over`
counts samples above that stage budget; the market-data budgets are 4us decode,
4us book, 4us fanout and 16us end to end.

## Seats, logs, canary and replay

| Call | Permission | Description |
| --- | --- | --- |
| `GET /api/ops/seats` | market | `{n,seats:[{id,name,broker,exchange,channel,state,enabled,orders,rejects,reconnects,window,last_us}],channels:[{name,seats}]}`; a single `paper` seat is reported when no gateway exists |
| `POST /api/ops/seats?action=start\|stop\|fault&id=1` | gateway | Start, stop or simulate a fault; returns `{code}` |
| `GET /api/logs/search?q=...&level=info\|warn\|error&kind=order\|risk\|ops\|market\|perf&since=N&limit=100` | ops view | `{n,lines:[{seq,ts_us,level,kind,text}]}`; case-insensitive substring, newest first, `limit` capped at 200 |
| `GET /api/canary` | market | `{state,gate_ok,routed,ok,rejects,reject_ppm,avg_slip,plan:{id,version,pct_ppm,min_samples,max_reject_ppm,max_slip}}` |
| `POST /api/canary?action=plan\|promote\|rollback&pct_ppm=&min_samples=&max_reject_ppm=&max_slip=` | gateway | Plans, promotes or rolls back; bucketing is a stable hash of the request sequence |
| `GET /api/replay?at_us=0&limit=20` | market | `{state,from_us,to_us,cursor_us,speed_ppm,progress_ppm,frames,ticks:[{ts_us,bid,ask,px}]}`; `at_us` defaults to the cursor |
| `POST /api/replay?action=load\|play\|pause\|stop\|seek\|step\|speed&from_us=&to_us=&at_us=&speed_ppm=&delta_us=` | ops view | The cursor is only written and advanced on the HTTP thread; the trading thread never touches it |

`load` without `from_us`/`to_us` takes the first and last timestamps of the first
series. `step` advances tape time in microseconds, not tick counts.

## Tape

The console's Data tab uses these calls to inspect a block tape on disk; any
other client with market permission can do the same.

| Call | Permission | Description |
| --- | --- | --- |
| `GET /api/tape/meta?path=run/tape.plt` | market | `{code:0,path,kind:"block|csv",versions,blocks,symbol,rows,bytes,first_ts,last_ts,last_version}` |
| `GET /api/tape/rows?path=run/tape.plt&version=0&since=0&until=0&offset=0&limit=100&cols=255` | market | `{code:0,version,rows,total,ticks:[{ts,bid,ask,last,bid_qty,ask_qty}]}` |
| `GET /api/tape/describe?path=run/tape.plt&version=0` | market | `{code:0,version,rows,cols:[{name,min,max,last,mean}]}` with eight columns |

`path` is resolved against the hub's working directory. A path that escapes it
with `..`, or a file that is not a block tape, answers `400`; a missing file
answers `404`. `kind` is `block` for a block tape and `csv` for a CSV dump.
`versions`, `blocks`, and `last_version` describe the commit chain, while
`rows`, `bytes`, `first_ts`, and `last_ts` describe the latest version.

`version` defaults to 0, which means the latest committed version; any other
value must be in `1..versions`. `since` and `until` are inclusive bounds on
`exch_us` and 0 means no bound. `offset` and `limit` apply after the time
filter, and `limit` is clamped to 500. `cols` is the tape column bitmask; 0 or
an omitted value selects every column, and only the requested columns are
emitted. In the reply, `ts` is the exchange timestamp `exch_us` in
microseconds, and `total` is the number of rows in that version before
`limit`.

```
curl -s -H "Authorization: Bearer $TOKEN" \
  'localhost:8080/api/tape/meta?path=run/tape.plt'
curl -s -H "Authorization: Bearer $TOKEN" \
  'localhost:8080/api/tape/rows?path=run/tape.plt&version=0&since=0&until=0&offset=0&limit=100'
curl -s -H "Authorization: Bearer $TOKEN" \
  'localhost:8080/api/tape/describe?path=run/tape.plt&version=0'
```

## Trading

| Call | Permission | Description |
| --- | --- | --- |
| `GET /api/status` | market | Sealed snapshot: `state`, `md_up`, `td_up`, `halted`, `note`, counters, risk (`net_qty`, `unrealized`, `equity`, `drawdown`, `rejects`), `rms` (same as `GET /api/risk/limits`), host, mode-filtered `pos` and `orders` |
| `POST /api/order` | trade | `{"name":1,"side":1,"qty":2,"px":0,"acct":"sim-test","off":0,"biz":0}`; `side` 1 buys, 2 sells; `px` 0 takes the opposite touch of that product; `off` 0–3 is open, close, close-today, close-yesterday, 4–6 the forced variants; `biz` 0–15 is the business axis (0 plain trade, 1–6 margin buy, short sell, buy-to-cover, sell-to-repay, subscribe, redeem, 7–15 local postings that the direct console order refuses) |
| `POST /api/basket` | trade | `{"legs":[{"name":1,"target":5},{"name":2,"target":-3}],"acct":"sim-test"}`; `target` is the desired net quantity; at most 8 legs per request |
| `POST /api/algo` | trade | Create: `{"action":"create","name":1,"side":2,"qty":4,"horizon_ms":2000,"slice_ms":500,"t0":0}`; sliced TWAP-style parent, `t0:1` opens then unwinds to the entry net. Control: `{"action":"pause\|resume\|cancel","id":7}`, or `{"action":"update","id":7,"qty":6,"horizon_ms":3000,"slice_ms":500}` (omitted fields keep their value). The response carries `id` |
| `POST /api/netting` | trade | `{"scope":1,"lot":1,"apply":0,"rows":[{"name":1,"side":1,"qty":2}]}`; `scope` 1 is account and counter, 2 is the account, 3 is the name; `apply=1` sends one IOC per net leg |
| `POST /api/unlock` | trade | `{"name":1,"long_qty":600,"short_qty":400,"lot":1,"clip":0,"px":0,"apply":0}`; pairs the smaller side, `clip` caps each leg |
| `POST /api/pair/order` | trade | `{"a_name":1,"a_side":1,"a_qty":1,"a_px":3500000000,"b_name":3,"b_side":2,"b_qty":1,"b_px":3400000000,"fx_ppm":1000000,"apply":1}`; sends A then B, `fx_ppm` only scales the spread preview |

`acct` is the account id from `run/accounts.conf` and must belong to the login
mode. A ticket that the risk layer refuses answers `code:1` with the reason in
`msg`, for example `rejected: short not allowed on 2 (0 available)` or
`rejected: no quote for name 3`.

## Research, backtest, and strategies

| Call | Permission | Description |
| --- | --- | --- |
| `GET /api/system` | market | Overview: `state`, `halted`, `mode`, `md_up`, `td_up`, `quotes`, `equity`, `drawdown`, `uptime_us`, `snapshot`, `gateways{total,running,failed}`, `strategies{total,running}`, `research{state,id,rows}`, `backtest{state,id}`, `alarms{critical,warn}`, `pre{rules,active,accts}`, `web{port}` |
| `GET /api/research` | market | Latest research job: `{code,id,state,started_us,done_us,rows_n,note,best,rows}` |
| `POST /api/research` | ops view | `{"tape":"...","name":1}`; `tape` defaults to the console tape; `202 {code:0,msg:"queued",id:N,state:"queued"}` |
| `POST /api/research/apply` | gateway | Writes `<ops.conf dir>/research.conf` from the best row: `{code:0,msg:"applied",path,kind,window,sign,threshold_ppm,horizon}` |
| `GET /api/backtest` | market | Latest backtest metrics plus `curve_n` and `curve:[{t,eq,dd}]`, at most 512 points |
| `POST /api/backtest` | ops view | `{"tape","name","kind","window","sign","threshold_ppm","qty","fee_ppm","cash"}`; `202 {code:0,msg:"queued",id:N,state:"queued"}` |
| `GET /api/strategies` | market | `{code:0,strategies:[...]}` for this mode |
| `GET /api/pair` | market | `{code:0,pairs:[...],n:N,packages:[{id,state,state_name,name_a,name_b,ratio_ppm,qty_a,qty_b,filled_a,filled_b,remain_a,remain_b,fx_ppm,note}],pkg_n:N}` for this mode; `pairs` come from the spread engine, `packages` are manual pair packages |
| `POST /api/pair` | trade | Manual pair package: `{"action":"create","name_a":1,"name_b":3,"qty_a":10,"ratio_ppm":1500000,"fx_ppm":1000000,"note":"..."}`; `update` takes `id` plus any of `qty_a`, `ratio_ppm`, `fx_ppm`, `note`; `cancel`, `close`, `fill` take `id`, and `fill` also `leg` (1=A, 2=B) and `qty`. Answers `{code,msg,id,state}` |
| `POST /api/strategy` | trade | `{"action":"add\|remove\|start\|stop\|pause\|resume","id":"s1"}`; `add` also needs `acct`, `instrument`, `kind`, `window`, `sign`, `threshold_ppm`, `qty`, `cooldown_ms`, and for kind 9 `instrument_b` and `ratio_ppm`; `202 {code:0,msg:"queued"}` |

Research and backtest requests go to one job-worker thread. Poll the matching
`GET` until `state` is `done` or `failed`. The worker reads a fixed request ring;
a full queue answers `503 {code:5,msg:"job queue busy"}`. A missing field answers
`400` with `code:1` and `missing <field>`; no token answers `401`; a missing
permission bit answers `403`. The sweep scores the first 70% of the tape and
picks its threshold and sign there. When no tape file exists the worker runs a
deterministic synthetic tape and marks `note` `synthetic`.

`kind` is 1..9: Return, Momentum, Volatility, ZScore, Ema, Rsi, Ofi, Imbalance,
Pair. `sign` is 1 or -1, `window` is 2..256, and `threshold_ppm`, `qty`, `fee_ppm`, and
`cash` are positive. A research row carries `kind`, `window`, `horizon`, `n`,
`ic`, `rank_ic`, `hit`, `turnover`, `icir`, and `score`; `best` adds `sign` and
`threshold_ppm`.

Strategy rows and counts are filtered by the session's mode, and a command that
names another mode's instance is refused by the registry. `start`, `stop`,
`pause`, and `resume` change one slot; `remove` clears it; `add` refuses a
duplicate id or a ninth instance. A row carries `id`, `state`, `state_name`,
`kind`, `window`, `sign`, `acct`, `instrument`, `threshold_ppm`, `qty`, `factor`,
`position`, `signals`, `orders`, `rejects`, `realized`, `unrealized`, `shared`,
`last_sig_us`, `started_us`, and `note`. `shared` marks an account and instrument
used by more than one instance, where PnL stays at account level. An instance
prices orders from its factor sign and threshold with `cooldown_ms` between
signals and sends them through `submit_manual`, so `Rms`, `Oms`, and `Pms` see
them like any manual ticket. A pair row keeps the single-leg fields but fills
them from the A leg and adds `instrument_b`, `ratio_ppm`, `position_b`,
`spread`, `mean`, `sd`, `pair_ready`, `rounds`, `wins`, `losses`, `timeouts`, and
`stop_hits`; `factor` carries the pair z-score in ppm. `GET /api/pair` renders
the same instances as a dedicated table.

`strategies.conf` sits beside `ops.conf`:

```
strategy = <id> <acct_id> <instrument> <kind> <window> <sign> <threshold_ppm> <qty> <cooldown_ms> [test|live] [b=<instrument_b> ratio=<ppm>] [auto]
```

`b=` and `ratio=` apply to kind 9; `auto` starts the instance on the first
trading beat so a deployed strategy runs through the console's tape replay.

It is read at startup; `add` and `remove` change this process only and do not
rewrite it. `/api/research/apply` writes `factor_kind`, `factor_window`,
`factor_sign`, `signal_ppm`, `horizon_bars`, `samples`, `ic_ppm`, `rank_ic_ppm`,
`hit_rate_ppm`, `turnover_ppm`, and `score_ppm` into `research.conf`; the file is
read on the next start or flow reload.

```
curl -s -H "Authorization: Bearer $TOKEN" localhost:8080/api/system
curl -s -H "Authorization: Bearer $TOKEN" -X POST localhost:8080/api/research \
  -d '{"tape":"run/tape.csv","name":1}'
curl -s -H "Authorization: Bearer $TOKEN" localhost:8080/api/research
curl -s -H "Authorization: Bearer $TOKEN" -X POST localhost:8080/api/research/apply
curl -s -H "Authorization: Bearer $TOKEN" -X POST localhost:8080/api/backtest \
  -d '{"kind":2,"window":16,"sign":1,"threshold_ppm":10,"qty":1,"fee_ppm":20,"cash":1000000}'
curl -s -H "Authorization: Bearer $TOKEN" -X POST localhost:8080/api/strategy \
  -d '{"action":"add","id":"s1","acct":"sim-test","instrument":1,"kind":2,"window":4,"sign":1,"threshold_ppm":10,"qty":1,"cooldown_ms":1000}'
curl -s -H "Authorization: Bearer $TOKEN" -X POST localhost:8080/api/strategy \
  -d '{"action":"start","id":"s1"}'
```

## Risk and operations

| Call | Permission | Description |
| --- | --- | --- |
| `GET /api/alarms?since=N` | risk view | Alarms newer than cursor `N` for this mode, plus the next cursor |
| `GET /api/risk/precheck` | risk view | Pre-trade limits, blacklist sizes, active-order count, halt flag, and per-account 1 s / 10 s order, cancel, notional, and ratio counters |
| `GET /api/events?since=N` | ops view | Audit records newer than `N`, plus the next cursor |
| `GET /api/gateways` | ops view | Gateways of this mode: `state`, `note`, `acct`, `auto`, `available` |
| `POST /api/gateway` | gateway | `{"action":"start\|stop","id":"sim-td"}`, same mode only |
| `POST /api/control` | risk control, reload gateway | `{"action":"pause|resume|halt|resume_risk|reload|cancel_all|stop"}`; `stop` is live-only |
| `GET /api/credit` | market | Credit terms per account of the mode: `{code:0,accounts:[{acct,id,class,ccy,source,asset_balance,total_debt,fund_debt,stock_debt,assure_ratio_ppm,finance_quota,finance_left,shortsell_quota,shortsell_left,long_qty,short_qty,special_short}]}`; `source` is 0 none, 1 counter, 2 manual |
| `POST /api/credit` | risk control | `{"action":"set","acct":1,"asset_balance":...,"total_debt":...,"fund_debt":...,"stock_debt":...,"finance_quota":...,"shortsell_quota":...,"assure_ratio_ppm":...}` or `{"action":"clear","acct":1}`; writes `<ops.conf dir>/credit.conf` |
| `GET /api/risk/limits` | risk view | Live in-process Rms limits and layer flags: `{code:0,rms:{max_order_qty,max_order_notional,max_pos_long,max_pos_short,max_daily_notional,price_band_ppm,tick_size,lot_size,max_orders_per_sec,max_live_orders,halted,precheck}}` |
| `POST /api/risk/limits` | risk control | Overrides only the fields present in the body, keeps the rest, refuses negatives: `{"max_order_qty":...,"max_order_notional":...,"max_pos_long":...,"max_pos_short":...,"max_daily_notional":...,"max_orders_per_sec":...,"max_live_orders":...,"price_band_ppm":...,"tick_size":...,"lot_size":...}`; posted to the trading thread as `OpsKind::RiskLimits` |
| `GET /api/users` | users | Accounts with role and enabled flag |
| `POST /api/user` | users | `{"action":"add|remove|role|password|enable|disable","user":"...","password":"...","role":"viewer|trader|risk|ops|admin"}` |

`cancel_all` pulls every working order on every gateway of the mode. `halt`
stops the strategy but leaves the gateways up; `stop` takes the trading
gateway down.

`POST /api/credit` is the manual/back-office channel while no credit adapter is
wired; once one exists it writes the same table with `source=1` and the console
needs no change. A zero `assure_ratio_ppm` is derived from assets over debt,
while a counter value is returned verbatim. Manual pair packages persist in
`<ops.conf dir>/packages.conf` and are restored on restart.

`/api/risk/limits` manages the Rms inside the trading thread: manual orders and
strategy children both pass through it per order, and `halt` stops there too.
Orders published straight onto the desk ring are treated as trusted producers
and skip Rms; that boundary is deliberate. The console can inspect and push
limits but does not move it.

## Clearing

| Call | Permission | Description |
| --- | --- | --- |
| `GET /api/clearing` | clearing | `{accounts:[report]}` with one entry per account of the mode |
| `GET /api/clearing?acct=1` | clearing | One account |
| `GET /api/clearing?acct=1&run=1` | clearing | Writes `run/clear-YYYYMMDD-<mode>.csv` and returns `{code,msg,path}` |
| `GET /api/clearing?acct=1&settle=1` | clearing | Writes the snapshot, marks open lots to the settlement price, rolls the day, returns `{code,msg,path,settle_us}` |
| `GET /api/clearing/trades?acct=1&since=0` | clearing | `{trades:[{seq,ts,acct,id,name,side,qty,px,fee,realized}]}` newer than `seq` |
| `GET /api/ledger` | risk | `{version,stamp_us,assets:[...],positions:[...]}` latest full counter snapshot; `?tier=`, `?acct=`, `?name=`, and `?direction=` narrow it, and a non-zero `direction` returns positions only. Amounts are whole units and prices use the 1e-4 scale |
| `POST /api/clearing/cash` | clearing | Body `{acct,action:"deposit"\|"withdraw",amount,ccy}`; returns `{code,msg,seq,acct}` |
| `GET /api/clearing/history?acct=1&days=8` | clearing | `{accounts:[{acct,id,days:[{day,settle_us,equity,unit_nav,cum_nav,pnl,realized,settled,fees,income,expense,trades,turnover,deposits,withdrawals}]}]}`, newest last, up to 32 |
| `GET /api/clearing/period` | clearing | `{accounts:[{acct,id,today,d7,d30,ytd}]}` |
| `POST /api/clearing/corp` | clearing | Body `{acct,name,event_id,kind,per10_cash,per10_stock,num,den,tax_ppm}`; `kind` 1 cash dividend, 2 stock dividend, 3 split/merge; returns `{code,msg,seq,acct}` |
| `POST /api/clearing/expense` | clearing | Body `{acct,action:"mgmt"\|"custody"\|"sales"\|"other"\|"interest",amount}`; a negative amount reverses; returns `{code,msg,seq,acct}` |
| `POST /api/clearing/shares` | clearing | Body `{acct,action:"subscribe"\|"redeem",shares}` at the pre-movement unit NAV; returns `{code,msg,seq,acct}` |
| `POST /api/clearing/reconcile` | clearing | Body `{acct,statement_a,statement_b,qty_tol,money_tol}`; reads the CSVs, returns `{code,path,count,qty_breaks,money_breaks,rows:[{name,present,qty:[i,a,b],equity:[i,a,b],dqty:[i-a,i-b,a-b],deq:[i-a,i-b,a-b],qty_bad,money_bad}]}` and writes the table |
| `GET /api/host/health` | risk view | `{code,stamp_us,level,bits,cpu_ppm,mem_ppm,load_ppm,threads,fds,disk_used_ppm,rx_bps,tx_bps,uptime_s,heartbeat_us,limits}`; heartbeat age is computed on read |

A cash request is applied by the trading thread on its next beat, so allow about
a second before reading the new balance. `amount` is in whole currency units.
A withdrawal larger than the current cash balance is dropped by the book.

Corporate actions, accruals, and share movements travel through the same queue
and also return a sequence number. `event_id` must be unique per instrument:
replaying the pair is a no-op. `per10_cash` is in 1e-4 currency units and
`tax_ppm` is the withholding rate (100000 = 10%); a split or merge scales
`new = old * num / den`. A negative accrual reverses an earlier booking. A
statement is a CSV of `name,qty,equity,realized,unrealized` (money columns may
be omitted), and `statement_b` is optional. In `/api/host/health`, `level` 0/1/2
means ok, warn at 80% of a limit, and critical at the limit, and `bits` is the
OR of cpu 1, memory 2, disk 4, fds 8, and heartbeat 16.

A report carries `acct`, `id`, `cash_in`, `deposits`, `withdrawals`,
`cash_balance`, `cash_flow`, `equity`, `equity_settle`, `available`, `realized`,
`realized_settle`, `unrealized`, `unrealized_settle`, `today_pnl`, `total_pnl`,
`pre_equity`, `commission`, `stamp`, `margin`, `market_value`, `trades`,
`turnover`, `day_*` counters, `day_settled`, `settle_us`, `day_seq`, `shares`,
`unit_nav`, `peak_nav`, `drawdown_ppm`, `cumulative_nav`, `distributions_ppm`,
`income`, `expense`, `mgmt_fee`, `custody_fee`, `sales_fee`, `fin_interest`,
`shares_in`, `shares_out`, `net_flow`, `fx_missing`, `days[]`, `cash[]`,
`rows[]`, and `recon[]`. Amounts are whole currency units; `unit_nav`,
`peak_nav`, and `cumulative_nav` are ppm of one unit. Each row is one product
with `long`, `short`, `long_today`, `long_yday`, `short_today`, `short_yday`,
`avg_long`, `avg_short`, `realized`, `realized_settle`, `unrealized`,
`unrealized_settle`, `fees`, `margin`, `mv`, `commission`, `stamp`, `trades`,
`turnover`, `ccy`, `fx_ppm`, `mark`, and `settle_mark`. Row amounts stay in the
row's currency; the totals convert to the account's base currency. `days[]`
holds the four most recent settled days in this endpoint (use
`/api/clearing/history` for up to 32), `cash[]` up to eight recent cash
movements, and `recon[]` the per-product difference against `Pms`, which is
empty when the books agree.

The CSV has one row per product, with columns
`mode,acct,name,ccy,long,short,long_today,long_yday,short_today,short_yday,avg_long,avg_short,realized,realized_settle,unrealized,unrealized_settle,fees,margin,mv,mark,settle_mark,commission,stamp,trades,turnover,cum_nav,income,expense,net_flow`.

## Health

`GET /api/health` answers `ok` with HTTP `200` and needs no session. Use it for
a load balancer or a process supervisor.

`GET /api/host/health` needs the risk-view permission and returns the sampled
host state. The trading thread samples once per second and raises one link
alarm when the level moves up; the endpoint recomputes the heartbeat age against
the last beat, so a stalled loop shows up even though no new sample is being
produced.

## Example

```
TOKEN=$(curl -s -X POST localhost:8080/api/login \
  -d '{"user":"admin","password":"pitlane","mode":"test"}' | jq -r .token)
curl -s -H "Authorization: Bearer $TOKEN" localhost:8080/api/instruments
curl -s -H "Authorization: Bearer $TOKEN" -X POST localhost:8080/api/order \
  -d '{"acct":"sim-test","name":1,"side":1,"qty":1,"px":0}'
curl -s -H "Authorization: Bearer $TOKEN" 'localhost:8080/api/clearing?acct=1'
```
