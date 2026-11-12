# pitlane API

这是调用说明。项目介绍在仓库根目录的 `README_CN.md`。英文说明是本目录的 `API.md`。对外调用有变动时，两份说明一起改。

包含一个头文件：

```cpp
#include "pitlane/pitlane.hpp"
```

所有名字都在命名空间 `pitlane` 里。链接 `libpitlane.a`，或者用 `-lpitlane` 链接动态库。动态库的 soname 是 `libpitlane.so.1`。两种链接方式使用同一套头文件。

发行版本是 `kVersion`，当前是 2.12.0，并拆成 `kVersionMajor`、`kVersionMinor` 和 `kVersionPatch`。`kDeskVersion` 是映射文件的布局版本，当前是 3。库的一次新发行可以保持布局版本不动。

带左侧分节导航的静态网页在 `doc/site/`（`python3 doc/gen_site.py` 生成）。打开**本地** `doc/site/index.html` 或 GitHub Pages；在 `github.com/.../blob/...` 里打开只会看到源码。启动期设置见 **启动配置**。

头文件分在 `pitlane/desk/`、`pitlane/book/`、`pitlane/order/`、`pitlane/time/`、`pitlane/study/`、`pitlane/util/` 和 `pitlane/host/` 下面。`version.hpp` 与 `pitlane.hpp` 同一层。下面的表在路径里写了区域。

价格和数量是整数。时间是微秒。方向 1 是买入，2 是卖出。普通拒绝通过 `false`、负数，或不是 `Why::None` 的 `Why` 表示。这些情况不抛异常。环或队列写满时，这次写入失败，已有内容保持不变。

## 启动配置

库在进程启动时接受的可调项，都放在可选的 INI 风格文件里（见 `examples/pitlane.conf`）。默认值是 `desk/config.hpp` 里 `PitConf` 的成员初值。省略某个键，或把值留空，就保留该默认值。未知键忽略。硬解析错误（截断行、该写数字却写了别的）时 `conf_load` 返回 false。

`conf_normalize` 在加载后只跑一次。它把环容量压到 `[2, 编译期 Cap]` 内的 2 的幂，把仅编译期定长的尺寸（`QueryFold`、`BrokerWatch::kCap`）恢复并写提示，并把每条调整写成一行可读文字放进调用方缓冲。启动时打印这些行。热路径不再打开配置文件。

```cpp
pitlane::PitConf conf{};
if (!pitlane::conf_load("/etc/pitlane.conf", conf)) return 1;
char hints[1024]{};
if (pitlane::conf_normalize(conf, hints, sizeof hints) > 0) {
    std::fputs(hints, stderr);
}

pitlane::MapOpt mop = pitlane::conf_map_opt(conf);
pitlane::MapInfo map{};
if (pitlane::map_create("desk.bin", pitlane::desk_bytes(), &map, mop) != 0) return 1;
pitlane::Desk* desk = pitlane::desk_view(&map, 1);
if (!desk) return 1;
if (!pitlane::desk_apply_conf(*desk, conf)) return 1;  // FanRing::tune，仅空环
```

`conf_map_opt` / `conf_arena_opt` / `conf_risk_caps` 把纠偏后的字段拷进映射、arena 与风控用的选项结构。`desk_apply_conf` 在环仍为空时对每条 desk 环调用 `FanRing::tune`。软容量不会超过编译期槽数组；磁盘上的 `Desk` 布局仍是 `kDeskVersion`。

| 节 | 键（摘要） |
| --- | --- |
| `[map]` | `hugepage`、`lock`、`prefault`、`numa_node`、`hugepage_dir` |
| `[arena]` | `hugepage`、`lock`、`prefault`、`numa_node` |
| `[desk]` | `intent_cap` … `query_cap`、`cash_cap`、`hold_cap` |
| `[broker]` | `max_qty`、`max_notional`、`max_live`、`*_limit_us`、`watch_cap`（仅提示） |
| `[query]` | `engines`、`accounts`、`queue`、`kinds`（仅提示；编译期定长） |
| `[host]` | `cpu`、`policy`、`priority`、`nice` / `set_nice`、`hugepages`、`thp_disable`、`mlockall`、`numa_node`、`irq_list` |
| `[shed]` / `[pace]` | StayShed / TokenBook–GCRA 默认值，由调用方构造 |

`conf_load_ex` 回报失败行号（从 1 起）。`conf_apply_host` 尽最大努力。`cash_apply_conf` / `hold_apply_conf` 软调行表上限。

## 打开共享区

一块文件映射就是整个工作区。写入进程创建它，其他进程挂到同一路径。

```cpp
pitlane::MapInfo map{};
if (pitlane::map_create("desk.bin", pitlane::desk_bytes(), &map) != 0) return 1;
pitlane::Desk* desk = pitlane::desk_view(&map, 1);
if (!desk) return 1;
```

`map_create` 创建或扩展文件，不会清掉已经存在的映射。默认 `MapOpt` 走显式大页：长度按 Hugepagesize 对齐，优先在 hugetlbfs（默认 `/dev/hugepages`，逻辑名映射为 `pitlane_<基名>`）建文件；路径本身已在 hugetlbfs 上则直接使用。不用 THP / `MADV_HUGEPAGE`。显式大页不可用时回退到调用方路径并置 `MapInfo::fallback`。默认尝试 `mlock`，`numa_node >= 0` 时对该区间做 `MPOL_BIND`。只有魔数是 `PITLANE1` 且布局版本是 `kDeskVersion` 时，`desk_view` 才返回工作区。当前布局版本是 3。第二个参数非 0 时，才会初始化一份仍然空白的文件。魔数不对的文件保持原样，调用返回空指针。魔数写上以后不要再次初始化。

其他进程用 `map_attach` 挂上，再把 0 传给 `desk_view`（须与创建时同一逻辑路径，以便解析到同一 hugetlbfs 文件）。`map_release` 解除映射；若曾 `mlock` 则先 `munlock`，再关闭文件。每条环只有一个写入者。读者不要共用一个读取位置。

`desk->clock_us` 是会话时钟，单位是微秒。`DeskLoop::beat` 把当次传入的时间写到这里。

## 发布和读取

`Desk` 有八条环：`intents`、`reports`、`quotes`、`algos`、`controls`、`pulses`、`prints` 和 `queries`。编译期容量分别是 `kIntentCap` 1024、`kReportCap` 1024、`kQuoteCap` 2048、`kAlgoCap` 256、`kControlCap` 256、`kPulseCap` 512、`kPrintCap` 512 和 `kQueryCap` 256。经 `desk_apply_conf` / `FanRing::tune` 后的软容量不超过该上限，由 `capacity()` 报告。资金和持仓在 `cash` 和 `hold` 里，各 256 行。

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

`Intent`、`Report` 和 `Quote` 在 `desk/types.hpp`。其余槽类型 `AlgoJob`、`ControlNote`、`Pulse`、`TapePrint` 和 `QueryNote` 在 `desk/wire.hpp`。比这些槽更大的数据，由调用方放在自己的环上，类型是 `Parcel<Bytes>`。长度大于 `Bytes` 时，`publish_parcel` 失败。

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

`QueryFold` 最多接受 4 个引擎和 32 个账号，每个引擎 16 条排队请求，查询种类最多 8。`push(engine, acct)` 保持仅按账号合并的旧路径。`push(engine, acct, kind, page)` 可带 page token；引擎、账号、kind 越界或队列满时失败。`fold` 合并队列。`pending` 每个账号列出一次。`pending_kind` 列出互异的 `(acct, kind)` 及最后一次 page；输出缓冲过短返回 -1。库不会在两页之间睡眠。下一次什么时候查，由调用方决定。

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

## 调用方自己保管的辅助函数

这些调用不挂到共享工作区上。存储由调用方保留，数字由调用方传入，结果怎么用也由调用方决定。

`IdSlip::issue` 给出下一个编号。这个编号仍是最新的一个，并且还没有调用 `keep` 时，`undo` 才能把它退回。`WireMold::seal` 把前缀抄进调用方的缓冲区。`put_u64` 和 `put_i64` 在这段前缀里的偏移上写入小端字节。`read_block` 接受长度至少为 `know` 的块，更短的块会被拒绝。`block_end` 跳过声明的块，包括这一版不读取的字节。

`StayShed::drop` 使用调用方自己的时间单位。停留短于目标时退出丢弃状态。第一次达到或超过目标时开始计窗口，这一条保留。窗口过去之后，仍然偏晚的条目被丢掉。停留再次短于目标时退出丢弃状态。

`floor_chunk` 把负数 tick 向远离 0 的方向对齐到块起点。`ChunkMap<Ticks,Blocks>` 按每块 `Ticks` 个价格保存数量，最多 `Blocks` 块。`Ticks` 取 8 到 256，并且是 64 的倍数。`Blocks` 取 1 到 64。数量 0 清掉这个 tick。只有正数量才会占用新块。目录已满时写入失败，原有数量保持不变。`chain_link` 把调用方自己的节点接到链尾，并返回槽位和复用计数。复用计数不一致时，`chain_unlink` 失败。`chain_zero` 让节点留在链上，数量为 0。`chain_take` 减少这个数量。

`ticket_pull` 和 `ticket_amend` 只从 `Working` 或 `Partial` 出发。每次都先留下原来的票据，再把版本加一。`ticket_rewind` 只在版本仍是这条命令写下的那个值时恢复快照。`ticket_fill` 也会把版本加一，所以成交会挡住尚未退回的命令。`PullWait` 期间的成交仍留在 `PullWait`。

`slice_open` 用时间跨度里的间隔数拆分母单数量，切段最多一百万。一段低于手数、而总量仍够一手时，整笔收成一段。最后一段带上余数。`slice_poll` 等到到期时刻才给出下一段。`slice_restore` 把没做完的子单退回池子。`slice_late` 先记入成交，能从池子里扣的先扣，返回值是从已经再次排出去的子单上收回的数量。`basket_make` 接受 `Create`、`Pause`、`Resume`、`Stop` 和 `Update`。种类最多 15 个字符。字节数大于 `max_bytes` 时失败。载荷本身不保存。

`recon_fill` 对同一个柜台成交编号只应用一次。本地还没有这张单时，用 `size` 建出来，先给出确认，再给成交。超过委托数量的部分记入 `over`，不加进 `filled`。重复编号给出 `Dup`，不改变订单。`recon_cancel` 把一张已知且未结束的订单标成已撤。

`FlightQueue<Cap>` 保存 2 到 64 条命令，按到期时刻排序。队列已满，或者编号、合约为 0 时，`push` 失败。`take` 只取出这个合约上已经到期的命令。

`BarFold` 忽略调用方标成不可交易的 tick，也忽略非正价格、负数量和负时间戳。桶起点是时间戳减去它对周期的余数。更晚的桶会收掉当前这根。更早的 tick 被拒绝，当前这根保持不变。空的周期不会被补出来。`seal` 交出还开着的那一根。

`act_mul` 用 `jump_den / jump_num` 作为价格乘数。现金分红只在 `income` 置上、并且前收高于分红时才改变这个乘数。`mul_fold` 只乘上 `ymd` 不晚于指定日期的公司行为。`scale_px` 向 0 截断。`traded_move` 返回去掉原始跳变之后的价格变化。1 股拆成 2 股时，跳变用 `1/2`。除权价正好是前收的一半时，交易变动是 0。

## 组帧、节奏和快照

这些调用不打开套接字，也不发单。字节、时钟和决定都留在调用方。

`digest_of` 只覆盖传给它的字节。`Sum8` 是求和的低 8 位。`Crc16` 是 CRC-16/CCITT-FALSE。`Crc32` 是 ISO-HDLC 的 CRC-32。`digest_write` 把这个值按小端写出去。`StreamAsm<Cap>` 把字节追加进自己的缓冲，`Cap` 从 16 到 65536。`FrameSpec` 在 4 到 32 字节的头里，用 `len_off` 处的小端长度读取正文。头、正文和校验都到齐，`pull` 才给出一帧。正文超过 `max_body`，或者整帧放不进缓冲，返回 `BadLen`，缓冲保持原样。校验不对时返回 `BadDig`，丢掉这一帧，后面的字节留下。好帧留在缓冲开头，直到 `commit`。帧还没取走时，`append` 失败。

`SeqGate<Cap>` 在旁边保留 2 到 64 个更晚的序号。`start` 设定下一个期望序号，0 会被拒绝。`live` 只在序号正好接上时放行。出现空洞时记下缺失区间，并把更晚的序号停进旁路。落在当前空洞里的实时序号被忽略。`replay` 只接受缺口里的下一个序号。缺口补齐后，连续的旁路序号一并放出。旁路里还有更后面的空洞时，再开一个新缺口。旁路已满，或者输出缓冲太短，门的状态不变。这个门不发送重传请求。

`token_open` 把桶装到 `burst`。每过 `per` 个时间单位，`token_take` 补充 `rate` 个令牌，存量不超过突发上限。时间往回走不补充。一次取用超过突发上限就失败。`gcra_open` 保存间隔和容忍量。`now` 加上容忍量达到理论到达时间时，`gcra_allow` 才通过，并把这个时间向前推一个间隔。`TokenBook` 和 `GcraBook` 保存 1 到 64 个键。键 0 被拒绝。每个槽都还在闲置时间以内时，新键被拒绝。闲置时间为 0 时不复用旧槽。第一次取用失败时，新键不会留下来。

`sign_ready` 在拒绝数为 0、并且通过数达到 `need` 时为真。已经拒绝之后，再通过也不能清掉拒绝。`sign_reset` 清掉两个计数，`need` 保持不变。开仓的 `hold_reserve` 扣掉数量，并占住一次撤单次数。平仓两样都不占。`hold_cancel` 只用于开仓记录，并退回未成交数量。只有 `rejected_here` 置上时才退回撤单次数。开仓全部成交，并且这次成交不是账户外来的，`hold_done` 才退回撤单次数。开仓超时的 `hold_late` 退回未成交数量和撤单次数。平仓超时只结束这条记录，不改额度。`hold_cancel_late` 退回撤单次数，持仓记录仍然留着，后面的全部成交不会再退一次。

`key_num` 和 `key_text` 把接口名和柜台键放在一起。数值键和文本键不相同。文本超过 23 个字符，或者接口名超过 15 个字符，写入失败，原来的键保持不变。`AckBook<Cap>` 保存 2 到 64 行。表满或者键已经存在时，`put` 失败。`link` 把等待下单确认的一行改成已关联。`arm_cancel` 从已关联状态开始撤单计时。`sweep` 只列出到期的本地编号，不改这些行。年龄大于自己的上限才算到期。距上次巡检还在 `gap` 以内时，这次不列出。输出缓冲太短时，巡检时钟不动。

`lat_mark` 只在打开打点、并且时刻不是负数时写入。任一节点还没有时刻，`lat_span` 就失败。`skew_sample` 保存柜台时间与本地时间之差的一半，整数除法向 0 截断。`skew_apply` 加上这个整数。`LatHist<Buckets>` 使用 4 到 32 个桶。0 和 1 落在 0 号桶。更大的值用不超过它的最高二次幂，超出最后一档的都留在最后一桶。`cum` 从 0 号桶累加到给出的桶。

`SnapPub<T, Slots>` 在对象里放 2 个或 3 个槽。一个写者调用 `begin_write`，填完后用 `publish` 让它可见。`pin` 让一个读者占住当前发布的槽。在 `unpin` 之前，第二次 `pin` 失败。只有 2 个槽时，读者还占着一个、另一个已经发布，再次写入就失败。有 3 个槽时，写者可以换掉一份已经发布、随后又被跳过的槽。库不启动线程。

`SigBook<Buckets, Keys>` 的桶数是 2 到 256 的 2 的幂，键是 1 到 1024 个。每个桶 8 个槽。签名 0 表示空槽，算出来是 0 就存成 1。签名写在键下标之后。`find` 返回从 1 开始的下标，没有则返回 0。`find4` 一次查四个键，比对前预取两侧桶。有 SSE2 时，一个桶的八个签名用一条向量比较。键已经存在时，`put` 返回真。两个桶都满时，一次插入最多把旧键往它的另一个桶挪 8 次。挪不完就失败，原来的键还在。插入侧一个写者。

`Grace<Readers>` 跟踪 1 到 64 个读者。`online` 记下当前代。`offline` 写成 0，这个读者不再挡住回收。调用方放下共享指针之后，用 `quiesce` 记下这一代。`bump` 把代加一，并返回新值。还有在线读者停在更老的代上时，`elapsed` 返回假，它不等待。调用方自己也在读者名单里时，要先 `quiesce` 再问 `elapsed`，否则自己仍算停在旧代。`RetireList<Cap>` 按放入顺序保存 2 到 64 个令牌。`reap` 从最老的开始放出已经过去的令牌，遇到第一个还没过去的就停。输出缓冲太短时，列表保持原样。列表不释放调用方的内存。

## 周期计数和定长低延迟表

这些调用不打开套接字，不发单，也不启动线程。存储和推进它的线程都由调用方持有。

`cycles_read` 读取周期计数。`cycles_ordered` 等前面的指令完成后再读。终点早于起点时，`cycle_gap` 失败。`load_fence`、`compiler_fence`、`cpu_relax` 和 `keep` 放在一次测量的两侧。`CycleProbe::arm` 指定要丢掉的预热次数。`begin` 先加栅栏再读。`end` 返回 1 表示留下这个样本，返回 0 表示仍在预热，返回 -1 表示读取失败或者计数往回走。

`UsSlot` 独占一条缓存行。`us_store` 用 release 写下微秒数。`us_load` 用 acquire 读取。这个槽由调用方写。探针打开期间，库不去取时钟。

`scale_set` 用调用方量到的周期数和纳秒数保存乘子和移位。两个数都必须是正数，周期数还要放得进宽除法。`scale_ns` 计算 `(cycles * mult + half) >> shift`，四舍五入到纳秒。`scale_span` 换算一段向前的周期差，终点更早则失败。

`DutyPace::open` 保存快档间隔、慢档间隔，以及一毫秒对应多少周期。`step` 返回跳过、快档、慢档或超时。它不去轮询描述符。`deadline_at` 把偏移加到起点上，加出会溢出就失败。期限还在前面时，`before_deadline` 为真。

`lap_ns` 把两个时刻相减。负数时刻表示还没写入，终点早于起点则失败。`hop_ns` 用记录写好的时刻减去触发它的时刻。`order_to_ack`、`tick_to_trade` 和 `tick_to_cancel` 对调用方已经记下的时刻用同一条规则。

`MagHist` 按数量级给每个桶记一个计数。值落在表外时，加入失败，原来的计数不动。0 分位是最小样本，100 分位是最大样本。中间的分位取跨过这个名次的那一格的下沿，名次向上取整。`mag_report` 填上个数、最小、最大、中位数、99 分位和 999 分位。`note_paced` 先记下这段耗时。耗时盖过不止一个间隔时，再把仍然不小于一个间隔的剩余逐段记下。最多记 64 笔。更长的停顿在这 64 笔之后返回 -1。回调返回假时，这一串就停。

`clock_publish` 把周期比例写到一页上。代先写成奇数，写完字段，再写成偶数。代是 0 表示这页还没发布过。周期数为 0，或者放不进宽除法，发布失败，页保持原样。`ClockView::refresh` 复制一份稳定的偶数代，并保存整数乘子和移位。页还没发布、这份比例组不起来，或者连续读到的代一直是奇数，就返回假。`to_wall` 用这份乘子和移位把周期戳换成墙钟，换算本身不再做除法，然后再把纳秒折进秒。

`TickWheel<Cap>` 保存 8 到 4096 个定时器。空闲链表和待挂栈各自独占一条缓存行。标签 0 被拒绝。`arm` 在调用它的线程上插入。`post` 可以在别的线程上调用，下一次 `take` 再把它们挂进轮子。不严格晚于当前拍的定时器会被改到下一拍。`take` 推进到给出的界限，并写出到期标签。缓冲不够就停下，剩下的定时器留在原地。`cancel` 去掉一个已经挂上的标签。`next_deadline` 是下一个有定时器的近桶，或者下一次把更远的桶收进来的拍。

`StageRing<T, Cap>` 使用 4 到 1024 个槽，容量是 2 的幂。每个槽都还被占着时，`acquire` 失败。阶段 0 的 `finish` 写入内容。`take` 只交出这一阶段的下一个序号。后占的序号先写完，也要等前面的序号。阶段 1 才发布已经释放的计数。只接受阶段 0 和 1。

`SeqWin<Cap>` 的窗口是 8 到 1024，并且是 2 的幂。令牌 0 被拒绝。`push` 返回 `In`、`Dup`、`Busy` 或 `Far`。重复的序号留下先到的令牌。离窗口远过两倍的序号是 `Far`，不会放进去。`pull` 先交出已经准备好的前缀，再交连续的窗口头，遇到空洞就停。滑动时旁边的队列放不下，`push` 返回 `Busy`，新令牌不保存。

`ReadWin` 跟踪 8 个读者。窗口小于 4 时，`open` 失败。`limit` 是已经加入的读者里最慢的位置再加上窗口。不是常驻的读者，落后超过窗口的四分之一，并且安静时间达到保持时长，就离开集合。一个读者都不剩时，`limit` 失败。这次调用不写调用方的字节。

`core_slot` 返回调用方自己的一块内存里某个核的地址。步长至少 64 字节，并且是 64 的倍数。核号超出个数则失败。

`Urgent` 独占一条缓存行，只放一个非 0 标签。槽里还有标签时，`post` 失败。`take` 返回这个标签，槽空时返回 0。

`TraceGate` 独占一条缓存行。`arm` 保存一个 16 位事件号和一个开关。`CoreTape<Cap>` 只在开关打开时写入。记录序号单独占一行。容量是 16 到 4096，并且是 2 的幂。写满以后覆盖最旧的一条。还没写过，或者已经被覆盖的记录，`at` 失败。`SegTape<Seg, N>` 有 2 到 16 段，每段 4 到 1024 条。下一段已经用 `give` 交回时，写满的这一段才封存。否则写者从当前段开头重写，覆盖次数加一，这次 `push` 返回假。`SpikeRing<Cap>` 只记录严格超过阈值的间隔。没有超过阈值时返回假，不算丢失。环满时，写者一侧的丢失计数加一，并返回假。读者序号单独占一行。

`FillRing<T, Cap>` 一个生产者、一个消费者，槽数是 2 到 1024。`claim` 返回下一个槽。已经有一次还没发布的领取，或者环已满，就返回空。`publish` 让这个槽可见。`cancel` 放弃这次领取，不发布。`take` 拷出下一个已经发布的槽。`take_span` 一次拷出多条，读取序号只向前推进一次。每个槽独占一条缓存行。

`SendJoinRing<T, Cap>` 多个生产者、一个消费者，槽数是 2 到 65536 的 2 的幂。下一格追上 head 时 `try_push` 失败。尾字高位是世代号、低 16 位是下标，环绕后旧值不会与 compare-exchange 匹配。占槽后再写载荷，`ready` 用 release 发布。`try_pop` 在 acquire 看到 ready 后才搬走，再推进 head。

`hex_encode` 写出小写十六进制，并在末尾放一个空字符。缓冲要同时容得下这两部分。缓冲太短时返回 -1，原来的文本不动。`hex_decode` 先检查每一位，再写入。长度是奇数，或者有一位不是十六进制，返回 -1，并且不写。`bytes_diff` 返回第一处不同的下标，两段相同返回 -1，指针或长度不能用时返回 -2。

`Mix64` 从调用方保存的种子往下取。`below` 会消耗一次取值。上界小于等于 1 时返回 0。更大的上界返回严格小于它的值。`span` 返回从下界到上界、包含两端的值，上界低于下界则失败。这个混合不是密码学生成器。

`TextBuf<Cap>` 保存 8 到 4096 字节，含末尾的空字符。`put`、`put_i64` 或 `put_hex` 失败时，原来的文本留下。`put_char` 拒绝值为 0 的字节。`put_hex` 的宽度是 1 到 16，值需要的位数更多就拒绝。

`SampleBox<Cap>` 保存 2 到 256 个样本。均值向 0 截断。百分位用向上取整的最近名次。0 分位是最小样本，100 分位是最大样本。盒子已满，或者总和会溢出，加入失败，新样本不写入。

`StepClock::step` 只向前走，负的步长或者会溢出的步长被拒绝。`jump` 可以跳到任意时刻。`FailBudget::arm` 允许那么多次成功，之后 `allow` 返回假。`StepList<Cap>` 要求标签非 0，时间不能早于上一次放入。`take` 拷出已到期标签的一个前缀。缓冲太短时，剩下的留下。`ladder_check` 接受空档。买档必须严格递减，卖档必须严格递增。价格或数量不是正数就失败。`intent_make`、`report_make` 和 `quote_make` 在拒绝时把输出清零。买价高于卖价的行情被拒绝。买价等于卖价可以留下。

## 一张委托单

`phase_text` 给出状态的名字。`sheet_open` 在调用方自己的单上写下本地编号、方向、价格和数量。编号是 0、方向不认识，或者价格或数量不是正数，就拒绝，原来的单留下。`sheet_bind` 记下一次柜台编号。再来一个不同的编号会被拒绝。

`sheet_apply` 按状态表推进这张单。状态表不允许的事件，所有字段都保持原样。成交数量大于还没完成的数量，同样整张不动。一次成交把剩余做完时，状态停在部分成交，剩余为 0。全部完成要再给一次结束事件。拒绝会记下调用方传入的原因。发送、接受和结束这几类事件只保留第一次非负的时刻。负数时刻不写。`sheet_ack_ns` 和 `sheet_done_ns` 把这些时刻相减。`sheet_line` 用一行替换文本缓冲，里面是编号、方向、状态、原因、数量、已成和剩余。本地编号放不进有符号 64 位时拒绝，原来的那一行留下。

`gate_ask` 对一次下单或一次撤单按顺序做检查，返回第一个原因。名单、通道规则、自成交表、资金行、持仓锁、风控上限、`BrokerWatch` 或流速窗口是空指针时，这一项跳过。限价下单要求价格和数量都是正数。名字在名单上就是拦截。通道不接受撤单时，返回撤单上限这一原因。通道不接受这种价格类型时，返回价格原因。通道的其他拒绝都是拦截。偏离只在限价下单、并且上限大于等于 0 时检查。自成交只在下单、并且表存在时检查。设置了 `cash` 与 `cash_need` 时走 `cash_enough`。设置了 `lots` 与 `lock_qty` 时只读检查持仓（`lock_future_ok` / `lock_cash_sell_ok`）。设置了 `risk` 时检查数量/名义/在途上限（有 `watch` 时走 `BrokerWatch::screen`）。前面这些检查都通过以后，才改流速窗口；任一更早失败都不改流速窗与在途侧状态。窗口已经到达上限时，下单返回流速原因，撤单返回撤单流速原因，窗口保持原样。这次调用不发单。

`risk_merge` 对每个正上限取更严（更小）的一侧，0 表示该轴关闭。`AcctRiskTab` 存账户级上限，未命中 fail-closed。`sign_begin(need)` 复位 `SignSlip` 并设定模块数，各模块再 `sign_pass` / `sign_reject`，最后 `sign_ready`（常见 need=3：全局/账户/通道）；拒绝一直保持到复位。

## 研究，以及调用方自己的决定

`feat_of` 读调用方已经拿着的盘口顶部。它给出价差、中间价、不平衡，以及一个微价格。微价格用对面的数量给本侧价格加权，除法走宽乘积，商是整数价格。两侧都没有数量时，用中间价。买价缺失，或者卖价低于买价，就拒绝。

`horizon_ticks` 把最多四个后来的价格换成整数跳，向零截断。后来的价格是负数时，这一档没有填写，结果写成 0。价格没变时结果也是 0。哪几档填过，由调用方自己知道。买入在后来的价格更高时算赚。卖出在后来的价格更低时算赚。

`RelBook` 给每个名字记一个标的，最多 64 个名字。同一个名字再次绑定会换掉标的。`related` 把指向同一个标的的名字抄进缓冲区，直到缓冲区满。`FactorBoard` 给 64 个名字各留四个最新值。再次写入只替换这一格。`Replay` 在调用方已经拿着的表上往前走。它不打开文件。

`study_step` 先应用一批行情，再按新的盘口顶部填写特征。盘口先更新，然后才判断特征。选择函数是空，或者选择函数返回假，委托单保持原样。选择函数返回一笔委托时，如果给了检查就先走 `gate_ask`，如果给了委托单再把它记成已暂存。被拒绝的委托不改委托单。给了委托单而本地编号是 0 时拒绝。这次调用不发单。调用方传入的函数，就是写策略的地方。库不决定方向，也不决定价格。

## 价格窗口、槽池和两笔费用

`PriceWindow<Cap>` 从下限价格起覆盖 `Cap` 个跳。`Cap` 是 64 到 4096 之间、64 的倍数。`reset` 写下限和步长，并清空窗口。`set` 写一侧的数量。数量 0 清掉这一跳。价格不落在步长上，或者落在窗口外，就拒绝，原有数量留下。`best` 给出最高的有量买价和最低的有量卖价。空档按一个机器字跳过。`shown_before` 把严格优于某个价格的数量加起来：买价看更高的，卖价看更低的。这个价格自己的数量不计入。加总会溢出时，输出保持原样并返回失败。

`publish_batch` 写入 1 到 `Cap` 条共用同一个读者掩码的槽，然后只存一次尾指针。有一条槽还忙着，调用返回 0。环里已经有的槽保持原样。`ack_span` 为一个读者连续确认若干位置，遇到确认不了的位置就停。已经确认过的位置保持确认。

`SlotPool<T, Cap>` 保存可平凡复制的记录。`Cap` 是 2 到 1024 之间的 2 的幂。`take` 拒绝编号 0、重复编号和已满的池。`drop` 把槽还给空闲栈。`find` 返回池内的指针。这个指针在该编号被丢掉之前有效。

`margin_both` 返回 `价格 * 数量 * 乘数 * 金额分子 / 金额分母` 和 `每手 * 数量` 里较大的一笔。参数不合法时返回 0。溢出时返回有符号最大值。`right_value` 里方向 1 是认购，2 是认沽。方向不认识，或者价格为负，返回 -1。

`FillRing` 是一个写入者和一个读取者。`claim` 返回一个可以原地填写的槽。`publish` 让这个槽可见。`cancel` 丢掉还没有发布的占用。`take` 抄出最早发布的槽。`take_span` 按调用方给的条数抄出若干槽，读取序号只存一次。每个槽独占一条缓存行。写入者记住上次看到的读取序号，读取者记住上次看到的发布序号。只有本端的副本显示环已满或已空时，才再次读取共享序号。环满，或者已经占着一个槽，再次占用就失败，已有内容不被盖掉。

`ExtremeWindow<Cap>` 保留最近 `Cap` 个正价格里的最高价和最低价。`Cap` 是 4 到 256 之间的 2 的幂。非正价格被拒绝，窗口保持原样。`clear` 丢掉全部价格。这个窗口不决定成交。

`TimeLine<Cap>` 按从旧到新保存同一个价格上的编号。`Cap` 是 4 到 256 之间的 2 的幂。`push` 拒绝编号 0、重复编号和已满的队列。`pop` 去掉最前面的编号。`erase` 去掉一个编号，其余编号的先后不变。拒绝时队列保持原样。

`cover_qty` 走调用方已经拿着的价格和数量。买入计入小于等于限价的价格。卖出计入大于等于限价的价格。买入和卖出是两条循环。在 SSE4.2 上每一步用一次打包比较处理两个价格，其余用四级展开的标量收尾。价格或数量不是正数就跳过。加总到 `need` 就停。数组保持原样。

`touch_lines` 对每一条缓存行做预取和加载，也处理最后一个字节。在 SSE2 上这次加载是 16 字节的打包读。空指针或长度是 0 时什么都不做。

`util/hint.hpp` 提供 `likely` / `unlikely`、冷热段布局、`assume_aligned`、缓存行长度与预取。在 x86_64 上打开 `PITLANE_NATIVE` 时增加 `-mprefer-vector-width=256 -mno-avx512f`。`util/simd.hpp` 提供打包的 `zero_i64`、`zero_i64_stream`、`copy_bytes`、`bytes_eq`，以及买入和卖出的 cover 辅助。构建定义了 AVX2 时，一次清零或比较四个 int64。缺少对应指令集时都走标量实现。`ByteArena` 为定容匿名 bump 字节 arena；默认以 `MAP_HUGETLB` 映射并将长度对齐到 Hugepagesize，不用 THP；内核未预留时回退普通匿名页并置 `fallback`，默认可再 `mlock` 与 NUMA bind；满容量时拒绝分配。`batch_size_for` 按积压给出自适应微批大小。`RankBits` 用字前缀加 popcount 得到稠密下标；`QuadTab` 把静态键装进每行至多四个键的缓存行表。`SpinLock` 是独占缓存行的 TTAS 自旋锁。`bound.hpp` 提供单次比较的下标检查与行走预算。`DagPlan` 以灰/黑染色求拓扑序。类型态票据以编译期标签使非法成交在类型上不可调用。`OwnPair` 保存调用方本地双边最优价。`SlotGate` 在 `on_drop` 之后忽略行情，直到 `seeded` 清掉 `need_seed`。`pick_cut` 与 `tandem_lat` 根据调用方已测得的间隔给出路径切分判定。`SeqPublishRing` 为无背压序号发布环，写端在读者未消费时可覆写槽位。`global_scale_set` 写入进程内共享的整数 TSC 周期换算系数。`DecideScratch` 暂存调用方装载的带评分候选意图。`host/` 提供 `sched_setaffinity`、调度策略与 nice、IRQ `smp_affinity_list`、`nr_hugepages` / `thp_disable`、`mlockall` 与 NUMA `mbind`；无权限时返回失败。

## 调用所在的头文件

| 头文件 | 内容 |
| --- | --- |
| `version.hpp` | 库的发行版本 `kVersion` |
| `book/bar.hpp` | 由调用方标成可交易的 tick 生成的 K 线 |
| `book/calendar.hpp` | 交易日和交易时段 |
| `book/cover.hpp` | 满足限价的可见数量 |
| `book/cumtick.hpp` | 由两帧累计成交量与成交额推算分价位手数 |
| `book/cross.hpp` | 限价簿和集合竞价 |
| `book/lags.hpp` | 时段时延和采样时延 |
| `book/ladder.hpp` | 固定价格窗口和有量的最优价 |
| `book/own.hpp` | 调用方本地双边最优价 |
| `book/quote.hpp` | 行情批次、有序 apply、逐笔记录和分钟滚动 |
| `book/steps.hpp` | 价位档和价格移动 |
| `book/venue.hpp` | `PaperVenue` |
| `desk/acct.hpp` | 交易员→多账户表与 `acct_idle` |
| `desk/broker.hpp` | `BrokerWatch`、`QueryFold` 与 `QueryWant` |
| `desk/risk.hpp` | `risk_merge` 与 `AcctRiskTab` |
| `desk/cash.hpp` | 资金/持仓板与 `cash_enough` |
| `desk/config.hpp` | 启动期 `PitConf` 加载、纠偏、host 应用、资金/持仓软上限 |
| `order/prelock.hpp` | 两阶段冻结 reserve / commit |
| `order/credit.hpp` | 负债空头池与 `option_free` |
| `study/fair.hpp` | 整数公允价、盘口偏置、压力分 |
| `util/breach.hpp` | 首次越界粘滞标志与池水位 |
| `desk/codec.hpp` | 品种、开平和方向 |
| `desk/feed.hpp` | 订阅掩码、价格缩放和原始行情 |
| `desk/ids.hpp` | `IdMint` |
| `desk/logline.hpp` | `Log` |
| `desk/names.hpp` | `NameBook` |
| `desk/region.hpp` | 工作区、`MapOpt` 文件映射和环容量 |
| `desk/ring.hpp` | `FanRing` |
| `desk/runtime.hpp` | `DeskLoop` |
| `desk/signal.hpp` | `PulseBoard` |
| `desk/types.hpp` | `Intent`、`Report`、`Quote`，以及方向、方式、阶段和 `Why` |
| `desk/wire.hpp` | 算法单、控制、信号、逐笔、查询和包裹槽 |
| `host/affinity.hpp` | 设置调用线程或进程的 CPU 亲和性掩码 |
| `host/hugepage.hpp` | `nr_hugepages`、Hugepagesize、`thp_disable` |
| `host/memlock.hpp` | 进程级 `mlockall` / `munlockall` |
| `host/irq.hpp` | 读写 IRQ `smp_affinity_list` |
| `host/numa.hpp` | NUMA 节点查询与区间 `mbind` |
| `host/priority.hpp` | `SCHED_FIFO` / `SCHED_RR` / `SCHED_OTHER` 与 nice |
| `order/ack.hpp` | 下单确认和撤单确认计时 |
| `order/bind.hpp` | 柜台单号和乱序回报 |
| `order/idclaim.hpp` | 定容 id 状态表（Wait / Live / Final） |
| `order/inbound.hpp` | 等到到期时刻再放出的命令 |
| `order/judge.hpp` | 名单、流速、比例、偏离和自成交 |
| `order/inflight.hpp` | 只列出超时在途标签的扫描 |
| `order/lane.hpp` | 通道规则、`LaneBook` 和 `Cause` |
| `order/basket.hpp` | 按合约的篮子目标账本 |
| `order/ledger.hpp` | 冻结、保证金、费用、成交回写、登录编号和在途槽 |
| `order/life.hpp` | 委托状态迁移和状态名字 |
| `order/line.hpp` | 同一个价格上编号的到达顺序 |
| `order/lots.hpp` | 手数拆分、整手校验与两融数量辅助 |
| `order/parent.hpp` | 母单时间切片和篮子控制头 |
| `order/plan.hpp` | 下一张单、可挂数量、切片和追价 |
| `order/pool.hpp` | 按编号查找的固定槽 |
| `order/recon.hpp` | 本地成交和柜台成交对齐 |
| `order/shed.hpp` | 按停留时长丢弃 |
| `order/sheet.hpp` | 委托单、部分撤与 `gate_ask` |
| `order/sign.hpp` | 多模块签字组合与开仓额度 |
| `order/ticket.hpp` | 命令快照和版本 |
| `order/typed.hpp` | 带编译期标签的票据类型 |
| `study/acb.hpp` | FIFO 手数队列与平均成本持仓账本 |
| `study/corp.hpp` | 公司行为的价格因子 |
| `study/decide.hpp` | 带评分候选意图的暂存与比较 |
| `study/extreme.hpp` | 最近一段固定价格里的最高价和最低价 |
| `study/plan.hpp` | 灰/黑染色因子拓扑序与固定动作表 |
| `study/study.hpp` | 盘口特征、事后偏移、关联、因子、回放，以及一次研究步骤 |
| `time/clock.hpp` | 发布出去的周期比例和墙钟 |
| `time/cut.hpp` | 依已测间隔判定是否切分路径 |
| `time/cycle.hpp` | 周期读取、整数比例和缓存的微秒 |
| `time/global.hpp` | 进程内共享的整数 TSC 周期换算系数 |
| `time/lap.hpp` | 调用方已经记下的时刻之差 |
| `time/mag.hpp` | 按数量级分桶和按间隔补记 |
| `time/sample.hpp` | 定长样本 |
| `time/simclock.hpp` | `SimClock` |
| `time/span.hpp` | 留在消息旁边的时刻 |
| `time/tape.hpp` | 开关、每核轨迹、封存的段和超阈值通知 |
| `time/trace.hpp` | 耗时样本、时钟差和分桶 |
| `time/urgent.hpp` | 不进队列的一个标签 |
| `time/wheel.hpp` | 时间轮 |
| `util/arena.hpp` | 默认显式大页的定容 bump 字节 arena |
| `util/batch.hpp` | 按积压自适应的微批大小 |
| `util/bound.hpp` | 单次比较的边界检查和行走预算 |
| `util/corebank.hpp` | 调用方自持的每核固定步长银行 |
| `util/chunk.hpp` | 稀疏价位块和撤单句柄 |
| `util/drill.hpp` | 测试用的时钟、预算、档位和普通记录 |
| `util/fillring.hpp` | 原地填写的槽 |
| `util/authbits.hpp` | MAC 白名单（≤3）与角色位测 |
| `util/frame.hpp` | 组帧、序号缺口和重排 |
| `util/grace.hpp` | 读者代和回收列表 |
| `util/hex.hpp` | 十六进制和逐字节比较 |
| `util/hint.hpp` | 分支提示、冷热段布局和预取 |
| `util/mix64.hpp` | 分裂混合 |
| `util/mold.hpp` | 预备报文、块长度和可以退回的编号 |
| `util/objpool.hpp` | 线程私有定容对象池 |
| `util/numeric.hpp` | 检查用的宽乘积 |
| `util/page.hpp` | 普通页与 Hugepagesize、按字节向上对齐 |
| `util/pace.hpp` | 令牌桶和理论到达时间 |
| `util/phase.hpp` | 有符号票号 PhaseGate；`flip_ready` 非阻塞等待上一轮排空；ActiveSum 轮换计数器 |
| `util/pinbank.hpp` | PrioSnap 优先级三槽缓冲；PinBank 按槽引用计数的多读者快照 |
| `util/readwin.hpp` | 最慢读者再加上一段窗口 |
| `util/seqgate.hpp` | 序号缺口恢复、`gap_width` 与 `reset` |
| `util/seqpub.hpp` | 无背压、可覆写未消费槽的序号发布环 |
| `util/seqwin.hpp` | 遇到空洞就停下的序号窗口 |
| `util/rankbits.hpp` | `RankBits` 稠密下标与 `QuadTab` 四键表 |
| `util/sig.hpp` | 签名桶 |
| `util/simd.hpp` | 打包清零、打包拷贝和 cover 辅助 |
| `util/snap.hpp` | 整份发布的快照槽 |
| `util/spin.hpp` | 独占缓存行的 TTAS 自旋锁 |
| `util/stage.hpp` | 按占槽顺序完成的环 |
| `util/stream.hpp` | 帧校验和字节流重组 |
| `util/sticky.hpp` | wait-free `StickyCounter`（写入后读侧可见最终值） |
| `util/stride.hpp` | 某一个核那一槽的地址 |
| `util/textbuf.hpp` | 定长文本 |
| `util/touch.hpp` | 按缓存行预取并加载 |
| `util/wirepx.hpp` | 尾数指数价格与定点换算 |
