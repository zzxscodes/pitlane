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
| `plan.hpp` | 推向目标仓的下一张单 |
| `ledger.hpp` | 冻结、成交回写、登录编号和在途槽 |
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
