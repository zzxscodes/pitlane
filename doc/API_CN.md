# pitlane API

这是调用说明。项目介绍在仓库根目录的 `README_CN.md`。英文说明是本目录的 `API.md`。对外调用有变动时，两份说明一起改。

包含一个头文件：

```cpp
#include "pitlane/pitlane.hpp"
```

所有名字都在命名空间 `pitlane` 里。链接 `libpitlane.a`，或者用 `-lpitlane` 链接动态库。动态库的 soname 是 `libpitlane.so.1`。两种链接方式使用同一套头文件。

价格和数量是整数。时间是微秒。方向 1 是买入，2 是卖出。普通拒绝通过 `false`、负数，或不是 `Why::None` 的 `Why` 表示。这些情况不抛异常。环或队列写满时，这次写入失败，已有内容保持不变。

## 打开共享区

一块文件映射就是整个工作区。写入进程创建它，其他进程挂到同一路径。

```cpp
pitlane::MapInfo map{};
if (pitlane::map_create("desk.bin", pitlane::desk_bytes(), &map) != 0) return 1;
pitlane::Desk* desk = pitlane::desk_view(&map, 1);
if (!desk) return 1;
```

`map_create` 创建或扩展文件，不会清掉已经存在的映射。只有魔数是 `PITLANE1` 且版本是 `kDeskVersion` 时，`desk_view` 才返回工作区。当前版本是 2。第二个参数非 0 时，才会初始化一份仍然空白的文件。魔数不对的文件保持原样，调用返回空指针。魔数写上以后不要再次初始化。

其他进程用 `map_attach` 挂上，再把 0 传给 `desk_view`。`map_release` 解除映射并关闭文件。每条环只有一个写入者。读者不要共用一个读取位置。

`desk->clock_us` 是会话时钟，单位是微秒。`DeskLoop::beat` 把当次传入的时间写到这里。

## 发布和读取

`Desk` 有八条环：`intents`、`reports`、`quotes`、`algos`、`controls`、`pulses`、`prints` 和 `queries`。容量分别是 `kIntentCap` 1024、`kReportCap` 1024、`kQuoteCap` 2048、`kAlgoCap` 256、`kControlCap` 256、`kPulseCap` 512、`kPrintCap` 512 和 `kQueryCap` 256。资金和持仓在 `cash` 和 `hold` 里，各 256 行。

`publish` 接收一条消息和一个读者掩码。掩码为 0 时，第一个确认的读者取走这个槽。其他掩码列出必须确认的读者，这些读者都确认之后槽才能再次使用。读者编号从 0 到 63。会话循环是读者 7，也就是 `kLoopReader`。

```cpp
pitlane::Intent in{};
in.tag = 1;
in.name = 1;
in.acct = 2;
in.px = 100;
in.qty = 5;
in.stamp_us = 1;
in.side = static_cast<uint8_t>(pitlane::Side::Buy);
in.act = static_cast<uint8_t>(pitlane::Act::Enter);
in.style = static_cast<uint8_t>(pitlane::Style::Day);

const uint64_t readers = 1ull << pitlane::kLoopReader;
if (!desk->intents.publish(in, readers)) {
    // 环已满，这张新单没有写入。
}
```

读者用 `at` 复制一个槽。位置是绝对序号，从 0 开始。这个读者还没有确认时，`fresh` 为真。`ack` 记下这次确认。`latest` 复制最新发布的一条。`tail` 是写入者下一次要使用的位置。

```cpp
pitlane::Intent got{};
bool fresh = false;
if (desk->intents.at(0, pitlane::kLoopReader, got, &fresh) && fresh) {
    desk->intents.ack(0, pitlane::kLoopReader);
}
```

`Intent`、`Report` 和 `Quote` 在 `types.hpp`。其余槽类型 `AlgoJob`、`ControlNote`、`Pulse`、`TapePrint` 和 `QueryNote` 在 `wire.hpp`。比这些槽更大的数据，由调用方放在自己的环上，类型是 `Parcel<Bytes>`。长度大于 `Bytes` 时，`publish_parcel` 失败。

## 跑会话循环

`DeskLoop` 以读者 7 读取各条环，触发定时器，并在看到 `kind` 为 1 的控制消息后停止。循环已经停止时，`beat` 直接返回。定时器编号 0 会被拒绝。`every_us` 为 0 的定时器只触发一次。定时器一共 32 个。

```cpp
void on_quote(void*, const pitlane::Quote&) {}

pitlane::DeskLoop loop;
loop.add_timer(1, /*when_us=*/1000, /*every_us=*/0);
pitlane::LoopHooks hooks{};
hooks.on_quote = on_quote;
loop.beat(*desk, /*now_us=*/1000, hooks);
```

钩子是函数指针：`on_quote`、`on_report`、`on_pulse`、`on_print`、`on_algo`、`on_query`、`on_control` 和 `on_timer`。只有你设置了的钩子会被调用。要让这个循环看到的消息，发布时必须带上读者位 7，或者使用掩码 0。

## 准备一张委托

`IdMint` 生成可以跨重启识别的订单号。字段是站点、种类、交易日个位、十秒槽和流水号，放在一个 64 位整数里。种类由调用方定义。

```cpp
pitlane::IdMint mint(/*site=*/1, /*kind=*/1);
uint64_t tag = mint.next(/*day_index=*/4, /*sec_of_day=*/10 * 3600);
```

`normalize_gate` 在委托入库前整理开平和方向。现货类强制开仓。上期所和能源中心把普通平仓改成平昨。其他期货交易所把平今和平昨收成普通平仓。线方向 0、2、5、6 是多头，1、3、4、7 是空头。限价为 0 时拒绝。结果里的 `Why` 说明原因。

```cpp
pitlane::GateForm form{};
std::snprintf(form.symbol, sizeof form.symbol, "rb2410.SHF");
form.side = static_cast<uint8_t>(pitlane::Side::Sell);
form.off = static_cast<int8_t>(pitlane::Off::Close);
form.px_mode = static_cast<int8_t>(pitlane::PxMode::Limit);
form.px = 3500;
form.qty = 2;
pitlane::GateResult gate = pitlane::normalize_gate(form);
```

`split_lots` 把一笔期货手数拆成几段。模式是 `TodayThenYdayThenOpen`、`CloseOnly` 和 `YdayThenStop`。其他模式返回 -1。超过最大手数的一段会被切开，小于最小手数的一段会被丢掉。

`commodity_plan` 和 `index_plan` 各自给出推向目标仓的下一张单。今仓和昨仓分开时，先平昨。`index_plan` 里的昨仓等于持仓减去当日已经开出的数量。`plan_px` 对买入取卖一，对卖出取买一。

`lock_future` 和 `lock_cash_sell` 冻结平仓或股票卖出需要的数量。`book_fill` 按成交写回今仓、昨仓和总仓。

## 在纸面场所撮合

`PaperVenue` 处理 48 个合约，常量是 `kNames`，以及 192 张未完成委托，常量是 `kTickets`。合约编号必须小于 48。一张新单要等报单时延过去，才按当时的盘口判断。撤单要等撤单时延过去。

```cpp
pitlane::VenueCfg cfg{};
cfg.lot = 1;
pitlane::PaperVenue venue(cfg);
const pitlane::Rung asks[] = {{100, 5}};
venue.set_levels(1, nullptr, 0, asks, 1);

pitlane::Report reps[4]{};
pitlane::VenueOut out = venue.admit(in, in.stamp_us, reps, 4);
```

`admit` 把回报写入调用方的缓冲区，并设置 `out.why`。`on_clock` 把时延已到的暂存单推进去，并完成到期的撤单。`on_print` 和 `on_tape` 减少挂单前面的数量。主动方向和挂单方向相同的逐笔不成交这张挂单。主动方向传 0 时，每一笔都保留。`on_shown` 处理后来的显示量变化，同一笔成交不会被计算两次。

`Style::Day` 打到限价时，剩余数量全部成交，可以多于该档显示量。`Style::Passive` 一旦会打穿就过期。`Style::Immediate` 只保留显示量。`Style::All` 在显示量不够整张时整张过期。同一账户的买卖互打时，这张单停止。

9 点 30 分之前，时延表是 0。之后 `session_band` 按 10:00、10:30、11:00、11:30、13:30、14:00、14:30 和 15:00 把一天分开。没有载入采样点时，`SimClock` 使用这九段。`SimClock::go` 返回负数时，表示这张单没有进入盘口。这个结果由调用方处理。`admit` 自己不读这只时钟。

`auction_clear` 选择成交量最大的价格。`tie_higher` 在成交量相同时取更高的价格。否则取更接近参考价的价格。

## 合约、信号和柜台

`NameBook` 最多保存 256 个合约。`insert` 写出从 1 开始的编号。符号已经存在或目录已满时返回 false。`link` 记录关系：0 相关，1 标的，2 衍生，3 跟踪标的，4 跟踪衍生。`set_session` 允许开市分钟晚于收市分钟，用来表示跨过午夜的时段。

`PulseBoard` 有 128 路订阅和 256 条队列。`watch` 建立一路订阅。没有匹配或队列已满时，`offer` 失败。`drain` 把队列里的信号复制出去。

`BrokerWatch::screen` 检查数量、方向、名义金额和在途笔数。`remember` 跟踪一笔已发出的委托。`expire` 去掉未及时确认、超过存活时间，或撤单后等待过久的委托。上限小于等于 0 时，这一项检查关闭。

`QueryFold` 最多接受 4 个引擎和 32 个账号，每个引擎 16 条排队请求。引擎、账号或队列超出范围时，`push` 失败。`fold` 合并这些队列，`pending` 把每个账号列出一次。库不会在两页之间睡眠。下一次什么时候查，由调用方决定。

`session_date` 把一个时刻归到它所属的交易日。8 点到 18 点不回退。18 点之后，周一回退 3 天，其余日子回退 1 天。凌晨则周一回退 2 天，其余日子回退 1 天。然后再跳过周末和 `ClosedDays` 里的日期。

## 辅助函数

`QuoteFold::apply` 写入一批 `Add`、`Set` 和 `Del`。其中一条不合法时，原有档位保持不变。`copy` 先给出最优价。`PrintLog::push` 只记录逐笔，不接收行情簿。`DayRoll::on_msg` 更新当前分钟。时间戳往回走时返回 false。时间单位是微秒。早于一天的分钟会被丢掉。

```cpp
pitlane::QuoteFold<8> book;
const pitlane::FoldEdit edits[] = {{pitlane::FoldOp::Add, 1, 10, 4}};
book.apply(edits, 1);
```

方向 1 是买盘，方向 2 是卖盘。价格已经存在时，`Add` 失败。`Set` 的数量为 0 时删掉这一档。`Del` 一个不存在的价格仍然成功。

`band_at_hold` 在价格正好等于下一档起点时留在较低的一档。`walk_up` 和 `walk_down` 从这一档开始走 tick。一步落到下一档起点之后，后面的 tick 用新档的步长。`clamp_px` 只在上限高于下限时夹价格。`span_index` 返回包含这一分钟的时段，没有则返回 -1。结束分钟不大于开始分钟的时段跨过午夜。`session_part` 对一个 `SessionRow` 返回开市、午休或休市。

`life_next` 返回下一个 `Phase`。从当前阶段不能接受这个事件时，结果是 `Phase::None`。撤单等待期间的成交仍留在 `PullWait`。`Resume` 和 `ResumePartial` 把被拒绝的撤单退回已报或部分成交。

`margin_need` 用乘数乘价格乘数量，再乘保证金分子，除以保证金分母。`charge_class` 对股票、债券、基金、科创板、指数和回购使用现货公式，其余品种使用费率公式。乘数和保证金比例由 `scale_of` 给出。

`rest_room` 是一条腿还能挂出的数量。两腿一起打时结果是 0。`cut_hedge` 在一条腿被削减后，按差额削减另一条腿。`slice_weight` 把一半进位，新的一段不大于 0 时返回 false。`align_leg` 先除以行情系数，再按最小变动价位四舍五入，一半进位。`pair_qty_ok` 对 688 和 689 要求至少 180，对其他沪深代码要求至少 80。`chase_px` 买入向上走，卖出向下走。

`IdBind::put` 写入一条待确认的对照。表已满或柜台键已经存在时失败。`confirm` 把它标成已关联。`erase` 留下空位，后面的 `put` 可以再用。本地号 0 会被拒绝。

订单还处在未设置、已发送或已确认时，`bind_fill` 把数量和金额累加到提前到达的字段。`bind_status` 把这些字段抄进订单。已确认并且已经有成交时，状态变成部分成交。柜台剩余少于本地剩余时，`bind_final` 先留住全成或已撤。`bind_catchup` 在剩余为 0 时落地这个状态。撤单尚未回来，且新价格等于成交价时，`quote_repost` 返回 false。

`lane_accepts` 读取 `LaneRule`。撤单要看 `pull`。新单要看价格类型和开平。`lane_can_amend` 读取 `amend`。`Cause` 从 10 起是字段检查，从 40 起是资金和持仓，从 70 起是风控，从 100 起是本进程以外。`cause_band` 返回这个分组。`cause_text` 是一个固定的短词。

`list_cause` 查开仓名单或平仓名单。`FlowWindow::allow` 把这一次也算进去，总数仍低于 `limit` 才通过。时间窗和传入的时刻使用同一单位。样本笔数还低于 `min_n` 时，`count_ratio_ok` 直接通过，比例刚好等于上限也通过。`cash_ratio_ok` 在金额比例等于上限时不通过。`cancel_ok` 和 `reject_ok` 在比例相等时不通过。`fill_ok` 在等于下限时通过。`px_shift` 是相对最新价的有符号距离，再乘上 `den`。`deviation_cause` 用这段距离的绝对值。`wash_hit` 要求同一合约、相反方向，并且价格相遇或打穿。账户相同，或者组号不是 0 并且相同，都算命中。

`ByteFrame::begin` 先留出四个字节。`end` 把正文长度写回去，并返回总长度。失败时返回 -1。缓冲区短于这个长度时，`frame_read` 失败。`seq_feed` 对下一个序号返回 0，跳过若干序号时返回正的缺口，旧序号返回 -1。第一个序号会启动这次监视。`SeqHold::start` 设定 `take` 下一步要放出的序号。`offer` 只保留接下来 `Cap` 个序号以内的消息。`SpanMark::mark` 拒绝负数时刻。任一节点还没有时刻时，`between` 失败。

## 调用所在的头文件

| 头文件 | 内容 |
| --- | --- |
| `region.hpp` | 工作区、文件映射和环的容量 |
| `ring.hpp` | `FanRing` |
| `types.hpp` | `Intent`、`Report`、`Quote`，以及方向、方式、阶段和 `Why` |
| `wire.hpp` | 算法单、控制、信号、逐笔、查询和包裹槽 |
| `runtime.hpp` | `DeskLoop` |
| `ids.hpp` | `IdMint` |
| `codec.hpp` | 品种、开平和方向 |
| `lots.hpp` | 把一笔数量拆成多张单 |
| `plan.hpp` | 下一张单、可挂数量、切片和追价 |
| `ledger.hpp` | 冻结、保证金、费用、成交回写、登录编号和在途槽 |
| `venue.hpp` | `PaperVenue` |
| `lags.hpp` | 时段时延和采样时延 |
| `simclock.hpp` | `SimClock` |
| `cross.hpp` | 限价簿和集合竞价 |
| `names.hpp` | `NameBook` |
| `signal.hpp` | `PulseBoard` |
| `broker.hpp` | `BrokerWatch` 和 `QueryFold` |
| `calendar.hpp` | 交易日和交易时段 |
| `feed.hpp` | 订阅掩码、价格缩放和原始行情 |
| `cash.hpp` | 资金行和持仓行 |
| `logline.hpp` | `Log` |
| `quote.hpp` | 行情批次、逐笔记录和分钟滚动 |
| `steps.hpp` | 价位档和价格移动 |
| `life.hpp` | 委托状态迁移 |
| `bind.hpp` | 柜台单号和乱序回报 |
| `lane.hpp` | 通道规则和 `Cause` |
| `judge.hpp` | 名单、流速、比例、偏离和自成交 |
| `frame.hpp` | 组帧、序号缺口和重排 |
| `span.hpp` | 留在消息旁边的时刻 |
| `numeric.hpp` | 检查用的宽乘积 |
