# NimbleFIX 重写设计评审：低延迟优先修订版

- 日期：2026-10-26
- 评审对象：[`rewrite-design.md`](rewrite-design.md)（2026-10-21 修订，1065 行）
- 评审前提：由 AI 实施；可以一次实现全部功能，但按可验收切片交付；低延迟为第一优先级；基准对手为 QuickFIX；Chronicle FIX 仅作架构与公开数字参考
- 文档性质：对设计提案的修订意见与验收建议，不替代 `rewrite-design.md`

## 0. 摘要

1. **方向成立，契约质量高。** §5 的所有权/失败语义、§7.3.1 的两种稀疏、§9.3 的“mmap ≠ 落盘”都写得很好，不需要推倒重来。
2. **低延迟优先会改变默认值。** 现有设计把 `queued` 和 `synced` 设为默认，等于把低延迟放在非默认路径上。建议改为 `inline` + `appended` 为默认，`queued` + `synced` 作为显式模式。
3. **工作量不是瓶颈，验证是。** AI 可以很快写完功能，但不能用断言证明延迟、崩溃一致性和协议边角；验收必须依赖独立 oracle、故障注入、perf 计数和可复现 benchmark。
4. **QuickFIX 只能证明“比它快”，不能证明“低延迟”。** 必须同时公布绝对数、空基线（minimal echo/codec）和完整测量条件；Chronicle 的数字不可直接比较。
5. **先冻结延迟关键接口，再扩功能。** 性能架构不能最后补；建议按“codec/slot → journal + inline session → 真实网络 → queued/多客户端 → 广度 → 优化”切片验收。

**怎么使用本文**：P0 项建议在批量实现前先改设计；P1 项可在对应切片内决策；附录 A 可直接对照修改设计文档。本文不重复设计文档的全部内容，只列需要修改或验证的决策。

## 1. 相比上一轮意见的修正

| 上一轮意见 | 在新前提下如何修正 |
| --- | --- |
| 阶段 1 太大，单人做不完 | 工作量不是约束；仍然拆，但目的改为“可验收切片 + 风险前置”，不是减工作量。 |
| 默认 queued 可以接受 | 低延迟优先：`inline` 应成为默认/旗舰路径；`queued` 作为隔离模式。 |
| `synced` 默认与低延迟冲突 | 结论加强：`synced` 不应 gate 默认发送；默认 `appended`/异步；给出耐久性矩阵。 |
| QuickFIX 作为独立对手方 | 保留互通价值；但不能作为低延迟证明；增加绝对基线、null baseline、公平性披露。 |
| 先砍 TLS/CLI/group 等范围 | 工作量允许保留；按风险和依赖排序后置，不因工作量砍功能。 |
| 文档砍成一页 ADR | 仍建议“短 ADR + 详细附录”；AI 能读长文，人验收需要短清单。 |

## 2. 优先级与延迟定义

### 2.1 决策顺序

建议把 §1 的顺序改为：

1. **正确性是门槛，不是可交易项。** 协议、恢复、无静默丢消息不能被延迟优化牺牲。
2. **延迟是第一优化轴。** 每个热路径决策都要有延迟预算和测量证据。
3. **易用性是约束。** 默认必须既安全又接近最快路径；不能让用户先成为性能专家。
4. **功能广度最后。** TLS、CLI、多版本、动态入口可以后置，但不能反向污染热路径。

注意：这不等于“永远选耐久性最高的方案”。耐久性是**显式声明的取舍**，只要不静默丢数据、不谎报保证，就不算正确性问题。

### 2.2 延迟分层

| 层级 | 边界 | 主要成本 | 证据 |
| --- | --- | --- | --- |
| L1 codec | 单条消息的 parse / validate / encode | 字段扫描、格式化、校验和、BodyLength | 微基准；分配/系统调用计数；wire diff |
| L2a inline session | wire → callback（acceptor）；send → wire（initiator）；含 journal append | 系统调用、解析、回调、编码、journal 内存写 | p50/p99/p99.9；每消息系统调用/分配；perf |
| L2b queued session | L2a + 跨线程唤醒/队列 | eventfd/futex、缓存行、排队 | p99；队列深度；唤醒延迟 |
| L3 end-to-end | 订单入口 → 执行回报 | 网络、应用逻辑、存储 | 场景基准；p99；吞吐-延迟曲线 |
| L4 recovery | 重启、resend、replay | 扫描、索引、重发 | 恢复时间；正确性；故障注入 |

测量边界必须写清楚：用户态 `recv` 前后、内核时间戳（`SO_TIMESTAMPING`/`SO_TIMESTAMPNS`）、硬件时间戳，三者不能混用。没有内核/硬件时间戳时，必须标注“用户态测量”，不能宣称 wire-to-callback。

### 2.3 性能契约

- 先建立 baseline，再定 SLO；不预先写 magic number。
- 每个数字必须带：硬件、内核、编译器/flag、CPU pinning、turbo/SMT、消息组合、速率、耐久性模式、warmup、运行次数、原始数据。
- 报告 p50/p99/p99.9/p99.99、最大值、吞吐@SLO、分配次数、系统调用次数、锁等待、page fault、cache/branch miss。
- 延迟基准必须 open-loop，避免 coordinated omission；用 HdrHistogram 或等价直方图。
- baseline 之后设回归预算（例如同吞吐下 p99 回退不超过某阈值）；普通 CI 做 smoke，专用机做 nightly。
- “inline 热路径零分配、零共享锁、稳态小消息一次 `send`（并正确处理 partial write/EAGAIN）”作为结构目标，用 allocator hook、`perf lock`、`strace -c` 验证，而不是靠代码审阅。

## 3. P0：必须修改的设计决策

### P0-1 耐久性：默认 `appended`，`synced` 不得 gate 默认发送

**现状**：§9.3 把 `synced` 设为默认，并要求相应记录及恢复 metadata 同步完成后，才允许协议发送或业务投递。

**问题**：如果 outbound 也按这个执行，每笔新单在写 socket 前都要等 fsync/msync。fsync 在 NVMe 上通常也是几十微秒量级，负载下可到毫秒级；这会直接摧毁低延迟定位，也让 headline benchmark 只能跑非默认配置。

**建议**：

- 默认改为 `appended`：append 到 mmap journal（内存写，页预 fault），然后发送；热路径不 fsync。进程崩溃可恢复；OS 崩溃/断电可能丢尾部，必须在文档和状态里明确。
- `synced` 改为可选，并区分两种语义：
  - `synced_send`：同步完成后才发送；最慢，适合明确要求“发前落盘”的合规场景。
  - `synced_async`：先发送，后台同步；receipt 的 synced watermark 后移；适合审计/可观测，不适合“发前落盘”承诺。
- group commit 作为 `synced` 的调度策略：一次 fsync 确认多笔；每笔在 receipt 中区分 appended / synced；批次大小、最大等待、backpressure 有上限。
- 明确 `none`（显式、不可恢复）是否允许：仿真/回测/纯延迟实验有真实需求；如果保留，必须在 API、日志、状态和文档里都标成不可恢复，不能成为隐藏 fallback。
- 写路径：
  - 优先考虑“直接在 journal record payload 里编码”，避免 frame buffer → journal 的二次拷贝；记录头、长度、checksum 在提交前回填，提交后用同一块内存 `send`。
  - 页预 fault / `fallocate` / 预分配 spare segment；segment 轮转、目录 fsync、索引更新移出热路径。
  - `mmap` 写 vs `pwrite + fdatasync` 由切片 B 的 journal 基准实测决定；mmap 的主要收益在 reader 侧和少一次 syscall，不要把“用了 mmap”当成低延迟证明。
  - 明确 SIGBUS、磁盘满、扩展失败、stale lock、torn tail、只读 inspect 并发的处理；文件系统白名单（ext4/xfs；NFS/overlay 不承诺持久性）。
- inbound 同样默认 `appended`：先 append 再 callback；这是内存写，成本可测；不要在低延迟名义下跳过 inbound 记录，否则恢复语义会崩。
- 记录提交顺序要写清：record header/length/checksum 校验通过并发布 commit marker（Release）后，才能 `send` 或 callback；不能让对端看到一条本机 journal 还未可见的消息。

**耐久性矩阵（建议写进 §9.3）**：

| 方向 | `none`（显式） | `appended`（默认） | `synced`（可选） |
| --- | --- | --- | --- |
| inbound | 不记录；崩溃丢消息；仅仿真 | 记录后回调；进程崩溃可恢复；OS 崩溃可能丢尾部 | 同步后回调；最慢 |
| outbound | 不记录；崩溃后可能无法重发 | 记录后发送；进程崩溃可恢复；OS 崩溃可能丢尾部 | 同步后发送；最慢 |
| transition | 不记录；恢复可能不一致 | 记录；进程崩溃可恢复 | 同步；最慢 |

**验收**：

- `strace -c` / perf 证明 `appended` 热路径每消息零 fsync/msync。
- kill -9 在每个 append/commit/send 边界注入，重启后逐条核对；不能只看文件存在。
- `appended` / `synced_async` / `synced_send` 三组 p50/p99/p99.9 与吞吐。
- 磁盘满、SIGBUS、segment 轮转、torn tail、stale lock 故障注入。

**文档修改**：§9.3 默认值与表格；§5.2 receipt 语义；§14 基准；§18 待定项。

### P0-2 默认执行策略：inline 是低延迟旗舰

**现状**：§6.1 默认 `queued`，§6.2 `inline` 是可选低延迟模式。

**问题**：`queued` 多一次跨线程唤醒/入队/出队，延迟和 jitter 都更高；如果 headline benchmark 用 `inline`、默认却是 `queued`，产品第一印象就是慢路径。

**建议**：

- 把 `inline` 设为 `Engine<Schema>` 的默认（或至少把 `latency` profile 设为默认）；`queued` 用显式类型/配置选择，定位为“隔离慢回调”的模式。
- 如果作者坚持 `queued` 默认，那么 quickstart、README 首例、所有 headline benchmark 必须用 `inline`，并在文档第一段说明“默认不是最低延迟路径”。否则低延迟定位不成立。
- `inline` 契约要写死：callback 必须有界、不可阻塞、不可调用可能阻塞的 engine API；提供 debug guard：测量 callback 时长，超阈值告警/关闭受影响的 session，并在测试里覆盖。
- `queued` 路径使用预分配内存 ring + eventfd（见 P0-3），不要用 journal reader 做稳态投递；暴露队列深度/高水位/唤醒延迟。
- 外部线程 `Sender` 不是最低延迟路径：最低延迟是在 I/O 线程/callback 内直接 `send`。`Sender`/producer lane 的成本要单独标注；不要把它宣传成“和 inline 一样快”。
- 两种模式共用同一 session core；不要让模板策略复制协议实现（见 P1-1）。

**验收**：

- 线程 ID/执行线程测试；callback 时长直方图；inline vs queued 的 p50/p99。
- 每消息分配次数、系统调用次数、锁等待。
- 慢 callback、阻塞 API、callback 内 stop、重复 stop、析构无死锁。
- inline 热路径零堆分配、零共享锁。

**文档修改**：§4 默认 API、§6.1/§6.2、§10.1 `dispatch` 默认、§16 quickstart。

### P0-3 投递路径：journal 是恢复记录，不是热路径队列

**现状**：§6.1 允许 queued 投递使用 mmap queue 的独立 reader cursor。

**问题**：把 journal 当投递通道会把 page fault、reader lag、retention 和磁盘行为带进 callback 路径；慢 reader 还会 pin segment，放大磁盘预算和 p99。

**建议**：

- `inline`：从接收缓冲区直接 parse + callback；journal append 只做恢复记录，callback 不经过 journal 读路径。
- `queued`：预分配 SPSC/MPSC ring + eventfd；append journal 成功后再入 ring；重启时从 journal replay 重建 ring/投递；delivery cursor 定期 checkpoint 到 journal/索引，保证 at-least-once（崩溃可能重复投递，业务需去重）。
- journal reader 只用于 resend、audit、recovery、backfill，不用于稳态业务投递。
- 如果保留“journal reader 投递”作为选项，必须和 ring 做同条件 benchmark，默认关闭。

**验收**：

- perf 统计热路径 page fault、minor/major fault、cache miss。
- reader lag、segment retention、慢 callback 下的磁盘增长。
- kill -9 后 replay 不丢不重（至少不静默丢；重复由业务去重）。
- ring vs journal reader 的 p99/吞吐对比。

**文档修改**：§6.1、§9.2、§13 测试。

### P0-4 基准：QuickFIX 是互通基线，不是低延迟证明

**现状**：§14 把旧版/QuickFIX 作为比较对象，§12/§13 把 QuickFIX 当可选测试依赖。

**问题**：QuickFIX C++ 是成熟但偏老的设计（`std::string`、`std::map`、默认单线程事件循环、频繁分配），打赢它不能证明低延迟；Chronicle 的公开数字来自 Java/JVM、不同硬件和工作负载，也不能直接对比；而且目前没有可直接链接的 C++ Chronicle FIX 库。

**建议**：建立“三层基准 + Chronicle 参考”，缺一不可。

1. **绝对基准**：不比较，只公布 L1/L2/L3 的 p50/p99/p99.9/p99.99、吞吐@SLO、分配/系统调用/锁/page fault、CPU/cache 计数，以及完整硬件/配置。
2. **空基线（null baseline）**：最小 epoll + 固定 FIX 报文 echo/parse-encode，不带 session/存储。用来分离“网络栈 + 最小 codec”和“引擎开销”；没有它，“比 QuickFIX 快 10 倍”说明不了什么。
3. **QuickFIX 基线**：用于互通和粗粒度对比。必须同机、同 pinning、同 TCP 选项、同消息组合、同 warmup、同日志关闭；披露 QuickFIX 的 store（`MemoryStore`/`FileStore`）和 NimbleFIX 的耐久性模式；同条件比较，不能拿 `appended` 对 `FileStore` 还宣称 durability 相同。
4. **Chronicle 参考**：只引用架构（单 writer、append-only mmap、reader cursor、无锁、预分配、busy spin），公开数字加 caveat，不放进同一张图。

**测量要求**：

- open-loop 负载、独立 load generator、pin 到不同核；HdrHistogram；warmup；多次运行取中位数/分位数；报告方差和异常值。
- 内核/硬件时间戳优先；没有就明确标注用户态边界。
- 延迟必须分 L1/L2/L3，不能把 fsync、恢复、TLS 混进“纯 codec”数字。
- 原始数据、脚本、命令行、配置进仓库；报告模板见 §7。

**验收**：

- benchmark harness 进 CI（smoke）和专用机（nightly）。
- 每次优化有前后对比和回归检查；不能只放“最好一次”。
- QuickFIX 互通测试和 benchmark 分离，避免 benchmark 代码污染产品路径。

**文档修改**：§14、§13.3/§13.4、§16 阶段 5。

### P0-5 发送契约：消除“接受后编码失败”

**现状**：§5.2 说编码/schema 错误在接受前返回；§7.3 说跨线程发送由 owner 最后编码。

**问题**：接受后仍可能编码失败（缓冲区不够、decimal 格式化失败、长度超限），这与“接受前返回”矛盾；在低延迟路径上更不能靠“先接受、后报错”掩盖。

**建议**：

- 二选一：
  - **预验证**：接受时完成最终帧长度上界、字段格式、容量检查；owner 侧编码成为 total function，失败只剩 store/transport。低延迟优先选这个。
  - **显式异步**：区分 `invalid`（接受前）与 `encode_failed`（接受后事件），并在 §5.2 错误表中写清；但不要让热路径出现“已接受后才发现编码错误”。
- callback 返回类型改为 `CallbackResult { handled / rejected(reason) / ignored }`；`send()` 失败单独返回/事件，不要用 `return sent.status()` 把一次 `queue_full` 升级成 session 级故障。
- 明确 reject 映射：业务拒绝 → `BusinessReject(j)` 还是 `Reject(3)`？按场景列表，不要留给用户猜。
- `SendReceipt` 的进度语义：accepted / appended / synced / written 分开；不要用“send 成功”暗示落盘或对端接收。

**验收**：

- 每个失败点注入：未 active、queue full、编码失败、journal 失败、socket 失败。
- 状态矩阵测试；无歧义返回值；错误码可重试性标注。
- 预验证路径不引入额外分配/二次编码（perf 计数）。

**文档修改**：§5.2、§4.2、§7.4。

## 4. P1：实现前应冻结的架构

### P1-1 shape / 模板 / 代码生成

- shape 必须是**有序集合**（sorted typelist），不是调用序列；否则 `.a().b()` 与 `.b().a()` 是两个类型/两份 plan，和 canonical wire order 冲突。
- `send<Message, Shape>(populate)` 用 `function_ref<void(Builder<Message, Shape>&)>` 擦除 lambda，保证每个 (Message, Shape) 一份 plan；不要让每个调用点实例化一套 encoder。
- `Engine<Schema, Inline>` 的类型参数会让实例数/编译时间/ABI 面翻倍；建议 strategy 构造期绑定，核心非模板，`Inline` 只是薄包装。
- codegen 提供“只生成实际使用的 message/field”子集模式；完整 FIX44 的生成物、编译时间、二进制大小要有预算。
- 编译期诊断要有 compile-fail 测试；`message<"D">` 的 class-type NTTP 支持要写清最低 GCC/Clang 和 fallback。
- 低延迟角度：确认生成的 encoder 内联、无逐字段虚调用、无每消息 `std::function` 分配；用汇编/perf 检查，不只靠模板“应该会优化”。

### P1-2 线程拓扑、CPU 与 poller

- 明确 owner 是“逻辑状态机”还是“线程”：建议 N 个 event loop，每个 loop 拥有多个 session；session→loop 分配、重连迁移、timer 集中、慢 session 公平性都要定义。
- `inline` callback 跑在 I/O 线程；`queued` 跑在应用 executor；CPU 预算、pin、isolcpus、`SCHED_FIFO` 由部署方决定，库不擅自设置。
- 低延迟 profile 支持 busy poll（`epoll_wait(0)`/`SO_BUSY_POLL`）、`TCP_NODELAY`、eventfd 唤醒、可选 `SO_TIMESTAMPING`；`io_uring` 等有数据再上。
- acceptor 多客户端要测：单 loop 多 session、跨 loop 隔离、慢客户端、连接数上限、fd/mapping 预算、noisy neighbor。
- 外部线程 `Sender` 的 MPSC 争用要单独 benchmark；producer lane 是进阶选项，不是默认路径。

### P1-3 journal 写路径与恢复

- 预 fault/预分配/`fallocate`；spare segment；轮转、目录 fsync、索引更新移出热路径。
- CRC32C 硬件；commit marker/checkpoint 的故障边界要逐点注入；torn tail 不越过猜序号。
- reader lease、segment 回收、retention、磁盘预算、SIGBUS/磁盘满、stale lock、文件系统白名单。
- `appended` 恢复保证是进程级，不是断电级；文档、状态、CLI 输出都要一致。
- 直接在 journal payload 编码（避免二次拷贝）要验证：记录头/长度/checksum 提交顺序、send 可见性、torn record 处理。
- 启动扫描时间、恢复时间单独报告；不要混进稳态延迟。

### P1-4 API / 配置 / 示例

- `on_session_active` 在恢复重连后会再次触发；提供 `context.is_recovery()` / `logon_count()`，示例不要无条件重复下单。
- §4.4 示例的 `order_qty().value_or(0)`、`exec_id = cl_ord_id` 是反模式；改成带 required 校验、Reject/BusinessReject 分支的完整示例。
- acceptor 认证不要用明文 `password_env` 比对；用 hash 或外部 authenticator，constant-time 比较；TLS peer 校验、证书重载、at-rest 脱敏补进安全模型。
- 配置统一 initiator/acceptor 的 `authentication` block；`schema`/`dictionary` 至少留 per-session override 字段，避免未来多 schema listener 破坏兼容。
- CLI 面按验收切片后置；hello-fix 提供 CMake 消费路径，不只 xmake。
- `estimate_resources(config)` + 容量表（session 数、消息率、磁盘/内存/fd/mapping），不要让用户自己猜。

### P1-5 协议矩阵

- 把 Logon（789 `NextExpectedMsgSeqNum`、141 `ResetSeqNumFlag`、98 `EncryptMethod`）、TestRequest/Heartbeat 超时、SeqNum too high/low（有/无 PossDup）、ResendRequest 分块、GapFill/Reset 接受条件、未知 MsgType 走 `Reject(3)` 还是 `BusinessReject(j)`、Checksum/BodyLength 错误处理，做成“实现并验证 / 不适用 / 未支持”表。
- 内置 Length/Data 配对表（90/91、93/89、95/96、212/213、354/355 等）；flat 模式才算可验收。
- session key 明确含 BeginString + Sender/Target CompID + 50/57/142/143 qualifier；FIXT.1.1 的 session/app 分离现在决定，别拖到阶段 4 改 core。
- QuickFIX 互通作为独立 oracle；旧仓库测试/边角案例迁成新测试（不抄实现），避免两边同错。

### P1-6 安全 / 可观测性 / 容量 / 兼容

- metrics 契约（Prometheus/StatsD 或稳定命名的 counters/gauges）、结构化 JSON 日志、订单关联 ID；热路径只做 per-thread 计数，不格式化。
- 错误码表：稳定 code、类别、可重试性、修复建议；CLI/JSON 一致。
- on-disk format 升级/迁移、source-compat/deprecation、experimental namespace、最低编译器、`-fno-exceptions`/`-fno-rtti` 支持矩阵。
- TLS 最低版本、peer 校验、证书重载、kTLS 可选；低延迟 profile 默认不启用 TLS，并明确测量差异。
- core dump/fork/signal 行为写清；库不安装信号处理器。

## 5. AI 实施的风险与控制

**工作量不是瓶颈，验证是。** AI 可以很快生成大量代码，但以下问题不会因为代码量增加而自动消失：

| 风险 | 控制 |
| --- | --- |
| 代码看起来对，但有隐藏分配/锁/系统调用 | allocator hook、`perf lock`、`strace -c`、`perf stat`；热路径结构测试 |
| 并发/生命周期有细微 bug | TSan、ASan/UBSan/MSan、确定性模拟、压力测试、callback 生命周期测试 |
| crash consistency 只测了“文件还在” | 每个 append/commit/sync/send 边界做 kill -9/截断/故障注入；逐条核对 |
| 测试过拟合实现 | QuickFIX/规范/属性测试作为独立 oracle；mutation testing；fuzz |
| benchmark harness 本身有错 | null baseline、内核时间戳、open-loop、coordinated omission 检查、独立复测 |
| API/ABI 漂移 | 冻结头文件、外部消费者编译测试、compile-fail 测试、版本化 |
| 过度设计/文档复制 | 每片只交付必要接口；用 benchmark 和用户故事裁剪 |

**每片验收包**（建议固定格式，方便人快速验收）：

1. 公开 API diff（头文件/配置/CLI）。
2. 不变量列表（协议、所有权、失败语义、线程）。
3. 测试报告（unit/property/fault/sanitizer/互通）。
4. Benchmark 报告（L1/L2/L3、原始数据、配置、前后对比）。
5. 已知限制/未支持项。
6. 一条可复现的 demo 命令。

**AI 可以一次写完，但不要一次合并。** 切片不是为了省工作量，而是为了让人能在一次 review 中判断“这片能不能接受”，并在架构走偏时尽早回退。每片都要能独立运行、独立测量、独立回滚。

## 6. 验收切片

| 切片 | 交付 | 必须的证据 | 明确不包含 |
| --- | --- | --- | --- |
| A. codec/slot/encoder | slot layout、静态 tag、generated setter、flat parse、frame encode、bench harness | L1 微基准；分配/系统调用计数；QuickFIX wire diff；编译时间/二进制/生成物大小；compile-fail 测试 | session、store、网络 |
| B. journal + inline session | mmap append/recovery、纯 session core、fake clock/transport、inline callback、appended 默认 | 协议状态机测试；kill -9 恢复；L2 p50/p99；appended vs synced；无 fsync 热路径 | 真实 socket、TLS、CLI |
| C. 真实网络 + 安装 | TCP initiator/acceptor、hello-fix、CMake install、外部消费者 | loopback 登录/下单/回报/登出；重启恢复；离开源码目录运行；安装包测试 | queued、多客户端规模、TLS |
| D. queued + 多客户端 + 背压 | ring + eventfd、queued executor、listener 多 session、慢 callback、容量上限 | 线程/顺序测试；饱和/背压；公平性；TSan；L2b p99；scale | TLS、CLI 完整面、FIXT |
| E. 广度 | TLS、CLI、group、XML/custom、dynamic entry、FIXT、manual mode | 功能/安全/配置/互通测试；文档示例可编译 | 性能优化收尾 |
| F. 优化与冻结 | profile 驱动优化、回归预算、发布文档、支持矩阵 | 完整 benchmark 报告；回归门禁；raw data；release checklist | 新功能 |

建议把 A 和 B 视为“架构验证”，C 是第一个用户可见 demo；D/E 可以并行开发，但合并 gate 仍按依赖顺序。每片都要有“可回滚点”和“已知限制”。

## 7. Benchmark 计划

### 7.1 边界与指标

按 §2.2 的 L1/L2a/L2b/L3/L4 分层报告，不混层。每层至少包含：吞吐、p50/p99/p99.9/p99.99、最大值、分配/系统调用/锁/page fault 计数、CPU/cache 计数、测试时长与样本数。

### 7.2 QuickFIX 公平性清单

- 同机、同内核、同 CPU pinning、同 turbo/SMT 设置、同 loopback/NIC。
- 同消息组合/大小/速率/心跳/日志关闭；同 warmup；同运行时长。
- QuickFIX 版本、编译 flag、store 类型（`MemoryStore`/`FileStore`）公开；NimbleFIX 的 durability 模式公开；同条件比较。
- 双方都调 `TCP_NODELAY`、buffer、线程数；不要只调一方。
- 交替运行，多次取中位数；报告方差；不要挑最好一次。
- 用户态 + 内核时间戳；没有就标注。
- 结论必须带绝对数（µs、msg/s）和条件；“快 N 倍”只能作为附注。

### 7.3 空基线

- 最小 epoll TCP echo，固定 FIX frame，不解析/不 session。
- 手写最小 parse/encode，不 session。
- 用来算 engine overhead；没有空基线，QuickFIX 对比不成立。

### 7.4 Chronicle 参考

- Java/JVM、不同硬件/负载，数字不可直接比较；目前也没有可直接链接的 C++ Chronicle FIX 库。
- 参考架构：单 writer、append-only mmap、reader cursor、无锁、预分配、busy spin、确定性 replay。
- 引用公开数字必须带 caveat；不要和 NimbleFIX 放同一张图而不标注。

### 7.5 报告模板

- 硬件/内核/编译/配置/负载/原始数据/脚本/限制。
- 一页结论 + 表格 + 复现命令。
- 原始数据（CSV/HdrHistogram）、benchmark 源码、运行脚本进仓库；报告注明 commit。

## 8. 需要作者确认的决策

1. 默认执行策略：`inline` 还是 `queued`？建议 `inline`（低延迟优先）。
2. 默认耐久性：`appended` 还是 `synced`？建议 `appended`；`synced` 显式。
3. 投递路径：ring 还是 journal reader？建议 ring；journal 仅恢复/重发/审计。
4. journal 写路径：mmap 还是 `pwrite + fdatasync`？由切片 B 数据决定；是否有偏好？
5. 是否允许显式 `none`/volatile 模式？建议允许但标成不可恢复，仅仿真/实验。
6. QuickFIX 对比的 store/配置：`MemoryStore` vs `FileStore` vs tmpfs？需要固定协议。
7. 参考硬件/SLO：有没有专用机？p99 目标大概量级？
8. TLS/CLI/group/FIXT 的 v1 范围：保留哪些？建议按切片后置，不砍。
9. 旧仓库测试/用例能否迁移？许可证/归属是否允许？
10. 最低编译器/标准库：GCC/Clang 版本、`<expected>`、class-type NTTP。

## 9. 建议的下一步

1. 先把 P0-1/P0-2/P0-3/P0-5 写回设计文档（默认值、耐久性矩阵、投递路径、发送契约），冻结接口；不要先写大量功能代码再补性能架构。
2. 建 benchmark harness + null baseline + QuickFIX 互通环境；先测绝对数，再谈对比。
3. 做切片 A（codec/slot/encoder）和 B（journal + inline session），交付完整验收包；这两片决定架构是否成立。
4. 用切片 C（真实网络 + hello-fix + 安装）做第一个用户可见 demo；再并行 D/E。
5. 在切片 F 之前不公开低延迟数字；所有公开数字必须来自可复现报告和已提交的 raw data。

## 附录 A：设计文档逐节修改表

| 章节 | 现状 | 建议 | 优先级 |
| --- | --- | --- | --- |
| §1 优先级 | 正确性 > API > 延迟 > 广度 | 正确性为门槛；延迟第一优化轴；易用性约束；广度最后 | P0 |
| §2 第一交付 | 含 nested group，与 §16 不一致 | 对齐切片；区分“首个 demo”和“首个 release” | P1 |
| §4.1/4.4 示例 | `value_or(0)`、`exec_id=cl_ord_id` | 改成带校验/拒绝分支的完整示例 | P1 |
| §5.2 send | 接受前编码错误 | 预验证 + total encode，或显式 encode_failed | P0 |
| §6.1 默认 queued | queued 默认 | inline 默认/旗舰；queued 显式 | P0 |
| §6.1 投递 | journal reader 可选 | 不用作热路径；ring + journal 恢复 | P0 |
| §7.3 shape | fluent 携带形状 | sorted set；function_ref；非模板 core | P0/P1 |
| §9.3 耐久性 | synced 默认 gate send | appended 默认；synced 可选；矩阵 | P0 |
| §9.4 journal | 通用描述 | 预 fault/in-place encode/rotation/SIGBUS | P0 |
| §10.1 配置 | password_env；全局 schema | hash/plugin；per-session override | P1 |
| §11 可观测性 | 事件/snapshot | metrics 契约 + JSON 日志 + 关联 ID | P1 |
| §12.1 构建 | xmake 主路径 | hello-fix CMake 消费路径 | P2 |
| §13 测试 | 四层 | 加 perf 计数、故障注入、QuickFIX oracle、soak | P1 |
| §14 性能 | QuickFIX/旧版比较 | 绝对 + null + QuickFIX 公平性；Chronicle caveat | P0 |
| §16 阶段 | 阶段 1 过大 | 按验收切片重排；风险前置 | P1 |
| §18 待定 | 默认值后定 | 先定延迟关键决策，其余后定 | P1 |

## 附录 B：热路径检查清单

每条消息（inline，acceptor/initiator）逐项检查：

- [ ] 无堆分配（allocator hook 计数为 0）
- [ ] 无共享锁/互斥量（`perf lock` 无争用）
- [ ] 无 fsync/msync（appended profile；strace 证明）
- [ ] 无日志/格式化/`snprintf`/locale
- [ ] 无异常抛出/捕获
- [ ] 无 `std::function` 每消息分配（SBO 或函数指针）
- [ ] 无 map/hash 查找（MsgType/字段用编译期表）
- [ ] 无逐字段 `std::string`
- [ ] 无未预 fault 的 journal 页
- [ ] 无 segment 轮转/目录 fsync
- [ ] 小消息稳态一次 `send`；必须处理 partial write/EAGAIN
- [ ] `TCP_NODELAY`；按需 `SO_BUSY_POLL`
- [ ] BodyLength/Checksum 一次计算，无二次扫描
- [ ] SendingTime 前缀缓存（按秒），只格式化毫秒
- [ ] metrics 仅 per-thread 计数，无锁
- [ ] callback 有界，超时 guard 生效
- [ ] 错误路径冷处理，不污染热路径

## 附录 C：延迟预算表模板

| 阶段 | 实测成本 | 测量方法 | 预算/备注 |
| --- | --- | --- | --- |
| `recv` 系统调用 | | `perf`/`strace` | |
| frame/parse | | 微基准 | |
| 字段校验 | | 微基准 | |
| journal append（inbound） | | 微基准/perf | |
| callback dispatch | | 微基准 | |
| 用户 callback | | 插桩直方图 | |
| encode | | 微基准 | |
| journal append（outbound） | | 微基准/perf | |
| `send` 系统调用 | | `perf`/内核时间戳 | |
| TCP/loopback | | 内核/硬件时间戳 | |

填完这张表，再谈“低延迟”才有依据。
