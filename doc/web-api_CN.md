# pitlane 网页 API

`client/index.html` 里的控制台以及任何其它客户端都通过 HTTP/1.1 JSON 访问
hub，端口由 `--web` 指定（默认 `8080`，只有设置 `--web-bind` 才对外监听）。
没有单独的管理端口。

所有路径都在 `/api/` 下。除 `/api/health`（纯文本）和 `/api/login`（也可用表单）
外，请求与应答都是 `application/json`。

## 会话与模式

`POST /api/login` 请求体 `{"user":"admin","password":"...","mode":"test"}`，或
`mode:"live"`；返回 `{"code":0,"token":"...","role":"...","perms":N,
"mode":"test","user":"..."}`。其余调用都要带
`Authorization: Bearer <token>`；网页同时会把它写进 cookie。
`POST /api/logout` 注销。

会话绑定登录时的模式。网关、账号、告警、持仓、清算列表都按该模式过滤；请求里
出现另一模式的账号或网关会返回 `409`，不会静默回退。`GET /api/me` 返回
`user`、`role`、`perms`、`mode`。

权限位决定调用能否成功，缺少权限返回 `403`：`viewer` 可看行情、风控与运维视图；
`trader` 增加下单；`risk` 增加控制、告警与清算；`ops` 增加网关控制；`admin`
增加用户管理。

错误统一为 `{"code":N,"msg":"..."}`，HTTP 状态为 `400`、`401`、`403`、`404`
或 `409`。单据与控制类接口在请求格式正确时返回 HTTP `200`，业务结果看
`code` 与 `msg`。`POST` 允许空正文，参数只在查询串里的动作即可这样调用；
查询串中的百分号编码会被解码。

## 行情

| 接口 | 权限 | 说明 |
| --- | --- | --- |
| `GET /api/symbols` | 行情 | 当前有行情的品种 |
| `GET /api/instruments` | 行情 | `{instruments:[{name,klass,ccy,tick,mult,margin_ppm,fee_ppm,min_fee,stamp_ppm,short,gw}]}` |
| `GET /api/accounts` | 行情 | `{accounts:[{num,id,name,class,ccy,cash,env,gateways:[...],td_running}]}`，只返回当前会话模式的账号 |
| `GET /api/series?name=1&tf_ms=60000&src=live&limit=240` | 行情 | `{bars:[{ts,open,high,low,close,volume}],last:{...}}`；`src=tape` 读盘上 tape |
| `GET /api/ticks?name=1&limit=300` | 行情 | 实时环里的最近逐笔 |
| `GET /api/history?name=1&since_us=0&until_us=0&limit=300` | 行情 | 从 tape 文件回看逐笔 |

`name` 是 `run/instruments.conf` 里的数字品种号。

## CTP 五档、逐笔与性能

CTP 行情适配器把每一帧深度数据依次解码、增量维护订单簿、重建逐笔、分发，
控制台只读同一个实时视图。未接 CTP（例如纸面模式）时 `available` 为 0，其余字段为 0。

| 接口 | 权限 | 说明 |
| --- | --- | --- |
| `GET /api/market/book` | 行情 | `{available,decoded,decode_err,prints,books,stale,crossed,rebuilds,fan_sinks,book:[{name,status,event,levels,exch_us,last,upper,lower,imbalance_ppm,snaps,updates,rebuilds,stale,crossed,bid:[[px,qty]×5],ask:[[px,qty]×5]}],ticks:[{name,aggressor,exch_us,px,qty,volume}]}`，`book` 为最近更新的四个品种，`ticks` 为最近 12 笔 |
| `GET /api/market/prints` | 行情 | 与 `/api/market/book` 同源，便于按语义取逐笔 |
| `GET /api/perf/hops` | 行情 | `{scale_on,md:{stages:[{stage,n,min_ns,p50_ns,p99_ns,max_ns,over}],total},ops:{...}}`；`md` 是行情线程（收包→解码→建簿→分发），`ops` 是控制台 beat 循环 |
| `GET /api/perf/baseline?tolerance_ppm=200000` | 行情 | 与基线逐项比较：`{rev,base_rows,cur_rows,regressed,diffs:[{name,base_p99,cur_p99,delta_ppm,regressed,missing}],current}` |
| `POST /api/perf/baseline` | 网关 | 把当前画像写成 `<conf 目录>/perf-baseline.txt`，返回 `{code,rows,path}` |
| `GET /api/perf/flame` | 行情 | `{nodes,depth,folded}`，folded stack 可直接喂 `flamegraph.pl` |
| `POST /api/perf/flame` | 运维视图 | 清空火焰累计 |

`p50_ns`/`p99_ns` 由幅值直方图给出，桶下界即返回值。`over` 是该跳超过预算的次数；
预算在行情端 `set_up()` 里设定（解码 4us、建簿 4us、分发 4us、端到端 16us）。

## 席位、日志、灰度与回放

| 接口 | 权限 | 说明 |
| --- | --- | --- |
| `GET /api/ops/seats` | 行情 | `{n,seats:[{id,name,broker,exchange,channel,state,enabled,orders,rejects,reconnects,window,last_us}],channels:[{name,seats}]}`；没有真实网关时给出一条 `paper` 席位 |
| `POST /api/ops/seats?action=start\|stop\|fault&id=1` | 网关 | 启停或模拟故障，返回 `{code}` |
| `GET /api/logs/search?q=...&level=info\|warn\|error&kind=order\|risk\|ops\|market\|perf&since=N&limit=100` | 运维视图 | `{n,lines:[{seq,ts_us,level,kind,text}]}`，`q` 为大小写不敏感子串，新→旧，`limit` 上限 200 |
| `GET /api/canary` | 行情 | `{state,gate_ok,routed,ok,rejects,reject_ppm,avg_slip,plan:{id,version,pct_ppm,min_samples,max_reject_ppm,max_slip}}` |
| `POST /api/canary?action=plan\|promote\|rollback&pct_ppm=&min_samples=&max_reject_ppm=&max_slip=` | 网关 | 下发计划、全量或回滚；按请求序号稳定哈希分流 |
| `GET /api/replay?at_us=0&limit=20` | 行情 | `{state,from_us,to_us,cursor_us,speed_ppm,progress_ppm,frames,ticks:[{ts_us,bid,ask,px}]}`；`at_us` 省略时用当前游标 |
| `POST /api/replay?action=load\|play\|pause\|stop\|seek\|step\|speed&from_us=&to_us=&at_us=&speed_ppm=&delta_us=` | 运维视图 | 回放游标只由 HTTP 线程改写和推进，交易线程不参与 |

`load` 不带 `from_us`/`to_us` 时取该模式第一路行情的首尾时间。`step` 的 `delta_us`
是向前推进的磁带时间，不是行情笔数。

## Tape

控制台的 Data 页签用这三个接口查看盘上的块存储 tape；任何有行情权限的客户端都可以调用。

| 接口 | 权限 | 说明 |
| --- | --- | --- |
| `GET /api/tape/meta?path=run/tape.plt` | 行情 | `{code:0,path,kind:"block|csv",versions,blocks,symbol,rows,bytes,first_ts,last_ts,last_version}` |
| `GET /api/tape/rows?path=run/tape.plt&version=0&since=0&until=0&offset=0&limit=100&cols=255` | 行情 | `{code:0,version,rows,total,ticks:[{ts,bid,ask,last,bid_qty,ask_qty}]}` |
| `GET /api/tape/describe?path=run/tape.plt&version=0` | 行情 | `{code:0,version,rows,cols:[{name,min,max,last,mean}]}`，共八列 |

`path` 相对 hub 的工作目录解析。用 `..` 越出该目录，或文件不是块存储 tape，返回
`400`；文件不存在返回 `404`。`kind` 为 `block`（块存储）或 `csv`（CSV 转储）。
`versions`、`blocks`、`last_version` 描述提交链，`rows`、`bytes`、`first_ts`、
`last_ts` 描述最新版本。

`version` 默认 0，表示最新提交的版本；其它取值必须在 `1..versions` 内。`since` 与
`until` 是 `exch_us` 的闭区间，0 表示不限。`offset` 与 `limit` 在时间过滤之后生效，
`limit` 上限为 500。`cols` 是 tape 的列位掩码；0 或省略表示全部列，应答只输出请求的列。
应答里的 `ts` 就是交易所时间戳 `exch_us`（微秒），`total` 是该版本在 `limit` 之前的
总行数。

```
curl -s -H "Authorization: Bearer $TOKEN" \
  'localhost:8080/api/tape/meta?path=run/tape.plt'
curl -s -H "Authorization: Bearer $TOKEN" \
  'localhost:8080/api/tape/rows?path=run/tape.plt&version=0&since=0&until=0&offset=0&limit=100'
curl -s -H "Authorization: Bearer $TOKEN" \
  'localhost:8080/api/tape/describe?path=run/tape.plt&version=0'
```

## 交易

| 接口 | 权限 | 说明 |
| --- | --- | --- |
| `GET /api/status` | 行情 | 快照：`state`、`md_up`、`td_up`、`halted`、`note`、计数器（含 `ack_timeouts`、`life_timeouts`、`pull_timeouts`）、风控（`net_qty`、`unrealized`、`equity`、`drawdown`、`rejects`）、`rms`（同 `GET /api/risk/limits`）、主机、按模式过滤的 `pos` 与 `orders` |
| `POST /api/order` | 交易 | `{"name":1,"side":1,"qty":2,"px":0,"acct":"sim-test","off":0,"biz":0}`；`side` 1 买 2 卖；`px` 为 0 取该品种对手价；`off` 取 0–3 表示开仓、平仓、平今、平昨，4–6 为对应的强平变体；`biz` 0–15 为业务轴（0 普通买卖，1–6 融资买入/融券卖出/买券还券/卖券还款/申购/赎回，7–15 为本地记账类，控制台直通单只接受 0–6） |
| `POST /api/basket` | 交易 | `{"legs":[{"name":1,"target":5},{"name":2,"target":-3}],"acct":"sim-test"}`；`target` 是目标净仓；单次最多 8 条腿 |
| `POST /api/algo` | 交易 | 建单：`{"action":"create","name":1,"side":2,"qty":4,"horizon_ms":2000,"slice_ms":500,"t0":0}`；切片父单，`t0:1` 先建仓再收回起始净仓。控制：`{"action":"pause\|resume\|cancel","id":7}`，或 `{"action":"update","id":7,"qty":6,"horizon_ms":3000,"slice_ms":500}`（缺省字段保持原值）。回包含 `id` |
| `POST /api/netting` | 交易 | `{"scope":1,"lot":1,"apply":0,"rows":[{"name":1,"side":1,"qty":2}]}`；`scope` 1 账户+柜台、2 账户、3 标的；`apply=1` 按净额逐腿发 IOC |
| `POST /api/unlock` | 交易 | `{"name":1,"long_qty":600,"short_qty":400,"lot":1,"clip":0,"px":0,"apply":0}`；按较小侧配对，`clip` 限制单腿 |
| `POST /api/pair/order` | 交易 | `{"a_name":1,"a_side":1,"a_qty":1,"a_px":3500000000,"b_name":3,"b_side":2,"b_qty":1,"b_px":3400000000,"fx_ppm":1000000,"apply":1}`；先 A 后 B，`fx_ppm` 只用于价差预览 |

`acct` 来自 `run/accounts.conf`，必须属于当前登录模式。被风控拒绝的单据返回
`code:1`，`msg` 里是原因，例如 `rejected: short not allowed on 2 (0 available)`
或 `rejected: no quote for name 3`。

## 研究、回测与策略实例

| 接口 | 权限 | 说明 |
| --- | --- | --- |
| `GET /api/system` | 行情 | 总览：`state`、`halted`、`mode`、`md_up`、`td_up`、`quotes`、`equity`、`drawdown`、`uptime_us`、`snapshot`、`gateways{total,running,failed}`、`strategies{total,running}`、`research{state,id,rows}`、`backtest{state,id}`、`alarms{critical,warn}`、`pre{rules,active,accts}`、`web{port}` |
| `GET /api/research` | 行情 | 最近一次研究：`{code,id,state,started_us,done_us,rows_n,note,best,rows}` |
| `POST /api/research` | 运维视图 | `{"tape":"...","name":1}`；`tape` 省略时用控制台 tape；`202 {code:0,msg:"queued",id:N,state:"queued"}` |
| `POST /api/research/apply` | 网关 | 把 best 行写入 `<ops.conf 同目录>/research.conf`：`{code:0,msg:"applied",path,kind,window,sign,threshold_ppm,horizon}` |
| `GET /api/backtest` | 行情 | 最近一次回测指标，外加 `curve_n` 与 `curve:[{t,eq,dd}]`，最多 512 点 |
| `POST /api/backtest` | 运维视图 | `{"tape","name","kind","window","sign","threshold_ppm","qty","fee_ppm","cash"}`；`202 {code:0,msg:"queued",id:N,state:"queued"}` |
| `GET /api/strategies` | 行情 | 当前模式的 `{code:0,strategies:[...]}` |
| `GET /api/pair` | 行情 | 当前模式的配对表 `{code:0,pairs:[...],n:N,packages:[{id,state,state_name,name_a,name_b,ratio_ppm,qty_a,qty_b,filled_a,filled_b,remain_a,remain_b,fx_ppm,note}],pkg_n:N}`；`pairs` 来自价差策略引擎，`packages` 是人工配对包 |
| `POST /api/pair` | 交易 | 人工配对包：`{"action":"create","name_a":1,"name_b":3,"qty_a":10,"ratio_ppm":1500000,"fx_ppm":1000000,"note":"..."}`；`update` 需 `id` 与可选的新 `qty_a`/`ratio_ppm`/`fx_ppm`/`note`；`cancel`、`close`、`fill` 需 `id`，`fill` 另需 `leg`（1=A，2=B）与 `qty`。返回 `{code,msg,id,state}` |
| `POST /api/strategy` | 交易 | `{"action":"add\|remove\|start\|stop\|pause\|resume","id":"s1"}`；`add` 还需 `acct`、`instrument`、`kind`、`window`、`sign`、`threshold_ppm`、`qty`、`cooldown_ms`，kind 9 另需 `instrument_b`、`ratio_ppm`；`202 {code:0,msg:"queued"}` |

研究与回测任务交给唯一的后台 worker 线程，轮询对应的 `GET`，直到 `state` 为
`done` 或 `failed`。worker 读取定长请求环，环满返回 `503`
（`{code:5,msg:"job queue busy"}`）。缺少字段返回 `400`（`code:1`，`missing <field>`）；
没有令牌返回 `401`；权限位不足返回 `403`。网格只在前 70% 的 tape 上评分，阈值和
符号也在这一段确定。tape 文件不存在时改用内置确定性合成 tape，并在 `note` 里标
`synthetic`。

`kind` 为 1..9：Return、Momentum、Volatility、ZScore、Ema、Rsi、Ofi、Imbalance、Pair。
`sign` 为 1 或 -1，`window` 为 2..256，`threshold_ppm`、`qty`、`fee_ppm`、`cash`
都取正数。研究行字段为 `kind`、`window`、`horizon`、`n`、`ic`、`rank_ic`、`hit`、
`turnover`、`icir`、`score`；`best` 另有 `sign` 与 `threshold_ppm`。

策略行与计数按会话模式过滤，命令指向另一模式的实例时会被注册表拒绝。`start`、
`stop`、`pause`、`resume` 改一个槽，`remove` 清空槽，`add` 拒绝重复 id 或第 9 个
实例。行字段包括 `id`、`state`、`state_name`、`kind`、`window`、`sign`、`acct`、
`instrument`、`threshold_ppm`、`qty`、`factor`、`position`、`signals`、`orders`、
`rejects`、`realized`、`unrealized`、`shared`、`last_sig_us`、`started_us`、`note`。
`shared` 表示同一账户与合约被多个实例共用，此时盈亏按账户口径。实例按因子符号与
阈值定价，两次信号之间至少间隔 `cooldown_ms`，订单走 `submit_manual`，因此
`Rms`、`Oms`、`Pms` 与手工单看到的是同一条链路。配对行沿用单腿字段并按 A 腿填
`position`，另加 `instrument_b`、`ratio_ppm`、`position_b`、`spread`、`mean`、
`sd`、`pair_ready`、`rounds`、`wins`、`losses`、`timeouts`、`stop_hits`；
`factor` 为配对 z 值（ppm）。`GET /api/pair` 以独立表格展示同一批实例。

`strategies.conf` 与 `ops.conf` 同目录：

```
strategy = <id> <acct_id> <instrument> <kind> <window> <sign> <threshold_ppm> <qty> <cooldown_ms> [test|live] [b=<instrument_b> ratio=<ppm>] [auto]
```

`b=` 与 `ratio=` 只在 kind 9 生效；`auto` 让实例在第一个交易节拍自动启动，随控制台
的 tape 回放一起完成。

它在启动时读取；`add` 与 `remove` 只改当前进程，不回写文件。`/api/research/apply`
把 `factor_kind`、`factor_window`、`factor_sign`、`signal_ppm`、`horizon_bars`、
`samples`、`ic_ppm`、`rank_ic_ppm`、`hit_rate_ppm`、`turnover_ppm`、`score_ppm`
写入 `research.conf`；下次启动或重开 flow 时生效。

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

## 风控与运维

| 接口 | 权限 | 说明 |
| --- | --- | --- |
| `GET /api/alarms?since=N` | 风控视图 | 该模式下游标 `N` 之后的告警，并返回下一个游标 |
| `GET /api/risk/precheck` | 风控视图 | 盘前规则上限、黑名单数量、活动单数、熔断标记，以及各账户 1 秒/10 秒下单、撤单、名义与撤单率计数 |
| `GET /api/events?since=N` | 运维视图 | `N` 之后的审计记录，并返回下一个游标 |
| `GET /api/gateways` | 运维视图 | 该模式网关：`state`、`note`、`acct`、`auto`、`available` |
| `POST /api/gateway` | 网关 | `{"action":"start\|stop","id":"sim-td"}`，仅限同模式 |
| `POST /api/control` | 风控控制，reload 走网关 | `{"action":"pause|resume|halt|resume_risk|reload|cancel_all|stop"}`；`stop` 仅实盘 |
| `GET /api/credit` | 行情 | 该模式每个账号的信用额度：`{code:0,accounts:[{acct,id,class,ccy,source,asset_balance,total_debt,fund_debt,stock_debt,assure_ratio_ppm,finance_quota,finance_left,shortsell_quota,shortsell_left,long_qty,short_qty,special_short}]}`；`source` 0 未接入、1 柜台、2 手工 |
| `POST /api/credit` | 风控控制 | `{"action":"set","acct":1,"asset_balance":...,"total_debt":...,"fund_debt":...,"stock_debt":...,"finance_quota":...,"shortsell_quota":...,"assure_ratio_ppm":...}`，或 `{"action":"clear","acct":1}`；写入 `<ops.conf 同目录>/credit.conf` |
| `GET /api/risk/limits` | 风控视图 | 进程内 Rms 的实时限额与分层标记：`{code:0,rms:{max_order_qty,max_order_notional,max_pos_long,max_pos_short,max_daily_notional,price_band_ppm,tick_size,lot_size,max_orders_per_sec,max_live_orders,halted,precheck}}` |
| `POST /api/risk/limits` | 风控控制 | 只覆盖请求里出现的字段，缺失字段沿用当前值，负数拒绝：`{"max_order_qty":...,"max_order_notional":...,"max_pos_long":...,"max_pos_short":...,"max_daily_notional":...,"max_orders_per_sec":...,"max_live_orders":...,"price_band_ppm":...,"tick_size":...,"lot_size":...}`；经 `OpsKind::RiskLimits` 下发给交易线程的 Rms |
| `GET /api/users` | 用户管理 | 账号、角色与启用状态 |
| `POST /api/user` | 用户管理 | `{"action":"add|remove|role|password|enable|disable","user":"...","password":"...","role":"viewer|trader|risk|ops|admin"}` |

`cancel_all` 撤掉该模式所有网关上的全部在途单。`halt` 停策略但保留网关；`stop`
停交易网关。

`POST /api/credit` 是柜台额度接口未接入时的人工/后台录入通道；接入信用适配器后，
适配器以 `source=1` 写入同一张表，控制台无需改动。`assure_ratio_ppm` 为 0 时按
`总资产/总负债` 推导，柜台回报的值原样返回。人工配对包写入
`<ops.conf 同目录>/packages.conf`，重启后恢复。

`/api/risk/limits` 管的是交易线程里的 Rms：手工单与策略子单都经它逐笔检查，`halt`
也停在这里。直接写入 desk 环的委托被当作可信生产者，不经过 Rms，这是有意保留的
分层；控制台能统一查看和下发限额，但不改变这条边界。

## 清算

| 接口 | 权限 | 说明 |
| --- | --- | --- |
| `GET /api/clearing` | 清算 | `{accounts:[report]}`，该模式每个账号一条 |
| `GET /api/clearing?acct=1` | 清算 | 单个账号 |
| `GET /api/clearing?acct=1&run=1` | 清算 | 写出 `run/clear-YYYYMMDD-<mode>.csv`，返回 `{code,msg,path}` |
| `GET /api/clearing?acct=1&settle=1` | 清算 | 先落盘，再按结算价盯市并日终结算，返回 `{code,msg,path,settle_us}` |
| `GET /api/clearing/trades?acct=1&since=0` | 清算 | `{trades:[{seq,ts,acct,id,name,side,qty,px,fee,realized}]}`，只返回 `seq` 之后的 |
| `GET /api/ledger` | 风控 | `{version,stamp_us,assets:[...],positions:[...]}` 最新的柜台完整快照；`?tier=`、`?acct=`、`?name=`、`?direction=` 用于过滤，`direction` 非 0 时只返回持仓。金额为整元，价格按 1e-4 定标 |
| `POST /api/clearing/cash` | 清算 | 请求体 `{acct,action:"deposit"\|"withdraw",amount,ccy}`，返回 `{code,msg,seq,acct}` |
| `GET /api/clearing/history?acct=1&days=8` | 清算 | `{accounts:[{acct,id,days:[{day,settle_us,equity,unit_nav,cum_nav,pnl,realized,settled,fees,income,expense,trades,turnover,deposits,withdrawals}]}]}`，按时间正序，最多 32 条 |
| `GET /api/clearing/period` | 清算 | `{accounts:[{acct,id,today,d7,d30,ytd}]}` |
| `POST /api/clearing/corp` | 清算 | 请求体 `{acct,name,event_id,kind,per10_cash,per10_stock,num,den,tax_ppm}`；`kind` 1 现金分红、2 送股、3 拆并股；返回 `{code,msg,seq,acct}` |
| `POST /api/clearing/expense` | 清算 | 请求体 `{acct,action:"mgmt"\|"custody"\|"sales"\|"other"\|"interest",amount}`；负数表示冲回；返回 `{code,msg,seq,acct}` |
| `POST /api/clearing/shares` | 清算 | 请求体 `{acct,action:"subscribe"\|"redeem",shares}`，按变动前单位净值成交；返回 `{code,msg,seq,acct}` |
| `POST /api/clearing/reconcile` | 清算 | 请求体 `{acct,statement_a,statement_b,qty_tol,money_tol}`；读取 CSV 后返回 `{code,path,count,qty_breaks,money_breaks,rows:[{name,present,qty:[i,a,b],equity:[i,a,b],dqty:[i-a,i-b,a-b],deq:[i-a,i-b,a-b],qty_bad,money_bad}]}` 并落盘差异表 |
| `GET /api/host/health` | 风控查看 | `{code,stamp_us,level,bits,cpu_ppm,mem_ppm,load_ppm,threads,fds,disk_used_ppm,rx_bps,tx_bps,uptime_s,heartbeat_us,limits}`；心跳年龄在读取时计算 |

出入金由交易线程在下一节拍应用，读取新余额前需等待约一秒。`amount` 为整币单位；
超过当前现金余额的出金会被清算簿丢弃。

公司行为、计提与份额申赎走同一条队列，也返回序号。`event_id` 在同一品种内必须唯一，
重复提交同一组视为重放。`per10_cash` 为 1e-4 币单位，`tax_ppm` 为代扣税率
（100000 表示 10%）；拆并股按 `new = old * num / den` 缩放。计提金额为负数表示冲回。
对账单是 `name,qty,equity,realized,unrealized` 形式的 CSV（金额列可省略），
`statement_b` 可选。`/api/host/health` 的 `level` 0/1/2 分别表示正常、达到阈值 80% 的告警、
达到阈值的严重；`bits` 是 cpu 1、内存 2、磁盘 4、文件描述符 8、心跳 16 的按位或。

报告字段：`acct`、`id`、`cash_in`、`deposits`、`withdrawals`、`cash_balance`、
`cash_flow`、`equity`、`equity_settle`、`available`、`realized`、
`realized_settle`、`unrealized`、`unrealized_settle`、`today_pnl`、`total_pnl`、
`pre_equity`、`commission`、`stamp`、`margin`、`market_value`、`trades`、
`turnover`、`day_*`、`day_settled`、`settle_us`、`day_seq`、`shares`、
`unit_nav`、`peak_nav`、`drawdown_ppm`、`cumulative_nav`、`distributions_ppm`、
`income`、`expense`、`mgmt_fee`、`custody_fee`、`sales_fee`、`fin_interest`、
`shares_in`、`shares_out`、`net_flow`、`fx_missing`、`days[]`、`cash[]`、
`rows[]`、`recon[]`。金额都是整币单位；`unit_nav`、`peak_nav` 与
`cumulative_nav` 是单位净值的 ppm。每行一个品种：`long`、`short`、
`long_today`、`long_yday`、`short_today`、`short_yday`、`avg_long`、`avg_short`、
`realized`、`realized_settle`、`unrealized`、`unrealized_settle`、`fees`、
`margin`、`mv`、`commission`、`stamp`、`trades`、`turnover`、`ccy`、`fx_ppm`、
`mark`、`settle_mark`。行内金额保持该品种币种，合计折算成账号的基准币种。
本接口的 `days[]` 只带最近四个已结算日（完整 32 条用
`/api/clearing/history`），`cash[]` 最多八条最近资金流水，`recon[]` 是与 `Pms`
的逐品种数量差异，一致时为空。

CSV 每个品种一行，列为
`mode,acct,name,ccy,long,short,long_today,long_yday,short_today,short_yday,avg_long,avg_short,realized,realized_settle,unrealized,unrealized_settle,fees,margin,mv,mark,settle_mark,commission,stamp,trades,turnover,cum_nav,income,expense,net_flow`。

## 健康检查

`GET /api/health` 返回 `ok`，HTTP `200`，无需会话，可用于负载均衡或进程守护。

`GET /api/host/health` 需要风控查看权限，返回主机采样结果。交易线程每秒采样一次，
等级上升时只推一条链路告警；接口在读取时重新计算心跳年龄，因此交易线程卡住、
不再产生新采样时依然能被发现。

## 示例

```
TOKEN=$(curl -s -X POST localhost:8080/api/login \
  -d '{"user":"admin","password":"pitlane","mode":"test"}' | jq -r .token)
curl -s -H "Authorization: Bearer $TOKEN" localhost:8080/api/instruments
curl -s -H "Authorization: Bearer $TOKEN" -X POST localhost:8080/api/order \
  -d '{"acct":"sim-test","name":1,"side":1,"qty":1,"px":0}'
curl -s -H "Authorization: Bearer $TOKEN" 'localhost:8080/api/clearing?acct=1'
```
