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

`SigBook<Buckets, Keys>` 的桶数是 2 到 256 的 2 的幂，键是 1 到 1024 个。每个桶 8 个槽。签名 0 表示空槽，算出来是 0 就存成 1。签名写在键下标之后。`find` 返回从 1 开始的下标，没有则返回 0。键已经存在时，`put` 返回真。两个桶都满时，一次插入最多把旧键往它的另一个桶挪 8 次。挪不完就失败，原来的键还在。插入侧一个写者。

`Grace<Readers>` 跟踪 1 到 64 个读者。`online` 记下当前代。`offline` 写成 0，这个读者不再挡住回收。调用方放下共享指针之后，用 `quiesce` 记下这一代。`bump` 把代加一，并返回新值。还有在线读者停在更老的代上时，`elapsed` 返回假，它不等待。调用方自己也在读者名单里时，要先 `quiesce` 再问 `elapsed`，否则自己仍算停在旧代。`RetireList<Cap>` 按放入顺序保存 2 到 64 个令牌。`reap` 从最老的开始放出已经过去的令牌，遇到第一个还没过去的就停。输出缓冲太短时，列表保持原样。列表不释放调用方的内存。

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
| `mold.hpp` | 预备报文、块长度和可以退回的编号 |
| `shed.hpp` | 按停留时长丢弃 |
| `chunk.hpp` | 稀疏价位块和撤单句柄 |
| `ticket.hpp` | 命令快照和版本 |
| `parent.hpp` | 母单时间切片和篮子头 |
| `recon.hpp` | 本地成交和柜台成交对齐 |
| `inbound.hpp` | 等到到期时刻再放出的命令 |
| `bar.hpp` | 由调用方标成可交易的 tick 生成的 K 线 |
| `corp.hpp` | 公司行为的价格因子 |
| `stream.hpp` | 帧校验和字节流重组 |
| `seqgate.hpp` | 序号缺口恢复 |
| `pace.hpp` | 令牌桶和理论到达时间 |
| `sign.hpp` | 检查计数和开仓额度 |
| `ack.hpp` | 下单确认和撤单确认计时 |
| `trace.hpp` | 耗时样本、时钟差和分桶 |
| `snap.hpp` | 整份发布的快照槽 |
| `sig.hpp` | 签名桶 |
| `grace.hpp` | 读者代和回收列表 |
