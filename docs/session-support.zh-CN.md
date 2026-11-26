# NimbleFIX 支持边界矩阵

[English](session-support.md) | **简体中文**

本文档说明 NimbleFIX 在协议、运行时、存储与运维方面支持的行为及明确边界。

> 相关文档：[使用手册](user-guide.zh-CN.md) · [架构设计](architecture.zh-CN.md) · [性能基准](benchmarks.zh-CN.md)

## 能力与边界总览

| 领域 | 支持的行为 | 明确边界 |
| :--- | :--- | :--- |
| **三层打包** | `NimbleFIX::nimblefix_core`（`DirectInitiator`、`DirectSendQueue`、遥测、回放）、`NimbleFIX::nimblefix_core_tls`（+ OpenSSL 3 `DirectTlsTransport`）、`NimbleFIX::nimblefix`（`Engine`、`ShardedEngine`、`IoBackend`、Acceptor）；可选 `nimblefix` CLI | 升级版本需重新编译依赖方；不提供跨版本 C++ ABI 稳定性保证。 |
| **协议版本** | `FIX.4.2`、`FIX.4.3`、`FIX.4.4`、`FIXT.1.1`（FIXT Logon 校验配置的 `DefaultApplVerID`） | 使用数字 IPv4/IPv6 地址；应用字典须与会话配置匹配；`FIXT.1.1` 负责会话层，`FIX50SP2` 提供应用层字典。 |
| **标准与自定义字典** | 完整 `FIX42`、`FIX43`、`FIX44`、`FIXT11`、`FIX50SP2` XML 代码生成（`nimblefix-schema-gen`）；`--messages` 子集裁剪与 `--from-fix-log` 日志驱动裁剪（SOH、竖线 `\|`、`.nfxj`）；生成 `switch(tag)` 单遍校验器 + O(1) `ParsedFieldIndex`；标量、枚举、组件、嵌套重复组、`DATA` 长度配对；支持 `1..999,999,999` 任意自定义 Tag | 字典校验覆盖结构与字段类型；交易所特有的业务规则由应用程序负责。 |
| **编码器与视图** | 流式 `StreamWriter`（`encode_stream` / `send_stream` / `enqueue_stream` / `try_push_stream`），支持 `if`/`for`、`std::optional`、动态 `w.header()`、自动 `DATA`/`LENGTH` 配对与嵌套重复组；`Fixed<ByteCap, fix::field::...>`；无字典 `nf::message<"D">()` 与 `DynamicMessage`；零拷贝 `FlatMessageView::is<M>()/as<M>()` | 视图与 `SessionContext` 仅在当次回调内有效，留存请拷贝。会话字段（`8/9/35/49/56/34/52/43/122/369/10`）由引擎管理；动态标准头字段（`50/57/142/115/128/...`）须在正文前通过 `w.header()` 写入。 |
| **校验能力** | 报文结构、必填字段、严格模式；字典枚举、精确 `Decimal`/整数/UTC 时间戳格式、重复组边界；每会话字典绑定（`SessionSettings::use_schema`）支持同引擎异构多字典；每会话 `UnexpectedFieldPolicy`（`reject` / `ignore_with_metric` / `passthrough`）；可选每会话 `verify_checksum = false`（默认 `true`）用于可信内网链路 | 无字典 Flat 模式（`FlatSchema`）在不绑定每会话字典时仅校验分帧与校验和。即使设置 `verify_checksum = false`，仍严格检查 `10=ddd<SOH>` 语法。 |
| **`DirectInitiator`** | 单会话客户端（`send_stream`、同线程 `enqueue_stream+flush` 合并 `sendmsg`、双线程无锁 SPSC `DirectSendQueue` + `drain`）；主备优先/轮询多节点切换（`endpoints` + `ConnectionStrategy`）；完整 FIX 状态机、分段 mmap 日志、崩溃恢复与 GapFill/Replay；可选 `DirectTlsTransport` | 每个实例对应 1 个 Initiator 会话；由调用方在 I/O 线程驱动 `poll_once()`、`drain()`、`reconnect_if_due()` 与 `poll_maintenance()`。 |
| **`Engine`、`ShardedEngine` 与 `IoBackend`** | 多会话运行时（`Engine`）与多核分片 Reactor（`ShardedEngine<Schema, Policy>`），支持单端口 Logon 路由移交、静态会话分片绑定、Linux CPU 绑核（`cpu_core`/`cpu_cores`）、可插拔 `IoBackend`（`epoll`/`busy_poll`/可选 `io_uring`）与 `IdleStrategy`（`sleep`/`busy_spin`/`hybrid_backoff`）；`Inline` 与 `Queued` 派发；多节点主备切换 | 每会话严格单写者。`ShardedEngine` 上不同分片的回调并行执行、同一会话内串行。`io_uring` 需显式开启编译选项（`--io_uring=y` / `-DNIMBLEFIX_ENABLE_IO_URING=ON`），未开启时请求返回 `ErrorCode::unsupported`。 |
| **跨线程发送** | `DirectInitiator`：无锁 SPSC `DirectSendQueue`。`Engine`/`ShardedEngine`：MPSC `Sender` 与无锁 SPSC `ProducerLane` | 仅保证同一生产者内部保序，不提供跨生产者或跨会话全局全序。跨线程误用 `ProducerLane` 返回 `wrong_thread`。 |
| **Logon 与认证** | Initiator/Acceptor、CompID 与版本校验、正数 `HeartBtInt`、`EncryptMethod=0`、凭据/自定义字段、回调认证 | Acceptor 在打开会话日志前完成认证。默认带凭据要求 TLS，除非显式设置 `require_tls=false`。持久化日志自动脱敏 Logon 密码。 |
| **Heartbeat 与 TestRequest** | 定时心跳、匹配 `TestReqID`、有限活性与写阻塞超时 | `Inline` 回调必须快速返回，C++ 用户代码不能被强制抢占。 |
| **SendingTime** | 每会话可配 `timestamp_precision`：毫秒（`ms`，默认）、微秒（`us`）或纳秒（`ns`）；UTC 日历校验；`Engine` 默认时钟偏差容忍 120 s；超差先发 Session Reject 再 Logout | 宿主机时钟同步（NTP/PTP）由运维负责。 |
| **缺口与重传** | `ResendRequest`、`GapFill`、向前 `SequenceReset` 重置模式、保留原序列号并带 `PossDupFlag=Y` 与 `OrigSendingTime` 的业务重传 | 重传绝不拆断半写帧；若所需历史已超出保留窗口则返回 `history_unavailable`，不伪造业务 `GapFill`。 |
| **序列号 Epoch 与运维控制** | 显式允许的 Logon 重置（`141=Y`）、审计 Epoch 记录、可选窗口边界自动重置（`reset_on_schedule_boundary`）；`Engine`/`ShardedEngine`/`SessionDriver`/`DirectInitiator` 支持线程安全 `request_logout`、`request_reconnect`、一次性 `request_sequence_reset` 与离线持久化 `adjust_sequences`（重传自动 GapFill） | 默认重连保留序列号；`adjust_sequences` 须在会话断开状态执行，回调低序号须开启 `allow_sequence_reset`；存在未处理输入或活跃读租约时禁止不安全重置。 |
| **FIX 369（`LastMsgSeqNumProcessed`）** | 发送已完成业务处理的最高连续入站序列号；校验对端进度；约束日志保留下界 | 已落盘但尚未完成回调与关联回复提交的入站消息不会提前通过 Tag `369` 确认。 |
| **Logout 与 Reject** | 本地、对端及双向并发 Logout（写完再关与有限关闭超时）；结构/类型错误返回标准 Session Reject（`35=3`） | 对手方特定的 `BusinessMessageReject`（`35=j`）策略由应用负责。 |
| **分段 mmap 日志** | 预分配分段 `MAP_SHARED` mmap 日志、SSE4.2 硬件 CRC32C、零拷贝 `reserve()`/`commit()`、`.tmp` + 原子 `rename` 建段、索引/检查点重建、段轮转、热路径外 `poll_maintenance()`、进程内带租约 `JournalReader` 与跨进程无锁只读 SPMC `JournalTailer`（`O_RDONLY`/`PROT_READ`，零写端开销） | 无纯内存模式；分段序号与 `RecordId` 单调递增永不复用；空间满时 fail-closed，绝不覆盖仍需重传的历史；跨进程 `JournalTailer` 绝不阻塞交易写端，滞后超出已回收分段时返回 `reader_expired` / `history_unavailable`。 |
| **恢复与确认语义** | 发送前落盘、回调前落盘；崩溃恢复重投未完成入站消息（`is_recovery == true`）并复用已落盘的关联回复 | 确认级别 `appended` 保障同机进程崩溃（`SIGKILL`/异常退出）恢复；热路径不做逐消息 `fsync`/`msync`，不覆盖宿主机突然断电；至少一次投递要求外部副作用幂等。 |
| **发送回执（`Engine`）** | `accepted`、`appended`、`written`、`failed`、`expired`，有界环形保留 | `written` 表示本机 socket/TLS 已接收字节；对手方确认以业务回报（如 `ExecutionReport`）为准。 |
| **TLS** | 可选 OpenSSL 3（`DirectTlsTransport` 与 `Engine`），TLS 1.2/1.3、CA + SAN 主机名/IP 校验、可选 mTLS、非阻塞 `WANT_READ`/`WANT_WRITE`、重连复用 `TlsContext` | 不支持交互式口令私钥；证书校验失败立即关闭连接，不降级明文。 |
| **会话标识、时间窗与套接字调优** | 持久化标识 = 协议版本 + 方向化 CompID + 可选限定符；显式 UTC 星期/时间窗口（支持跨午夜与可选 `reset_on_schedule_boundary`）；`SocketOptions` 支持数字源 IP/端口绑定、`TCP_QUICKACK`、`SO_SNDBUF`/`SO_RCVBUF` 与尽力而为 `SO_BUSY_POLL` | 会话显示名与端口号不属于持久化标识；不做夏令时换算；`reset_on_schedule_boundary` 默认关闭且须同时开启 `enabled` 与 `allow_sequence_reset`。 |
| **旁路 mmap 遥测** | 可选 `telemetry_path`（`.nfxt` `MAP_SHARED`）与 `telemetry_stage_probes`，支持 `DirectInitiator`、`Engine`、`ShardedEngine`；433 桶单写者纳秒 HDR 直方图（`read`/`parse`/`validate`/`journal`/`write`、`p50/p90/p99/p99.9/p99.99`）；只读 `TelemetryReader` + `nimblefix telemetry` | 默认关闭（零开销）；单写者保证热路径零锁、零堆分配、零 `fsync`。 |
| **历史回放** | `<nimblefix/replay.hpp>`（`JournalReplayer::replay` / `replay_to_tcp`）与 `nimblefix replay`；按路径读取在线或离线 `.nfxj` 时使用跨进程无锁 `JournalTailer`（不抢占 `LOCK_EX`），也提供进程内带租约 `JournalReader` 重载；支持方向/序列号/Epoch/管理帧过滤、`ReplayOptions::wire_hints` 自定义 `DATA`/`LENGTH` 配对及全速/原速/倍速回放至回调或 TCP 端点 | 原样回放已提交的 FIX 报文。 |
| **手动事件循环** | 调用方线程驱动 `Engine::start()` + `poll_once(max_wait_ms)` 或 `DirectInitiator::poll_once()` / `drain()`；线程安全 `request_stop()` | 每个 `Engine` 实例对应单次运行生命周期；重启请基于同一存储目录重新创建。 |

---

## 默认资源与存储上限

| 参数 | 默认值 | 说明 |
| :--- | ---: | :--- |
| 心跳间隔 | 30 s | 每会话可配 |
| 建连 / Logon / TLS 握手超时 | 10 s | 防止无限挂起 |
| 重连退避 | 250 ms | Initiator 自动重连 |
| 写阻塞 / 排队超时 / 读暂停 / 投递超时 | 30 s | `max_pending_age_ms=0` 关闭出站超时过期 |
| Logout 完成超时 | 5 s | 有界优雅关闭 |
| 单条 FIX 报文上限 | 64 KiB | 管理消息预留 8 个发送槽 + 2 倍最大帧字节预算 |
| `Engine` 容量 | 64 会话、32 待认证连接 | 256 条发送队列 / 4 MiB、128 条待处理输入 / 4 MiB |
| `Engine` Sender / 回执 / Queued 工作池 | 256 条 Sender 命令 / 4 MiB、1,024 回执、2 工作线程（128 回调 / 4 MiB） | 可在 `EngineConfig` 中调整 |
| 日志预算（每会话） | 总容量 16 MiB、单段 1 MiB、65,536 索引记录、保留 8,192 条出站消息 | Epoch 审计预留 1 MiB；单库最多 65,535 段与 1,024 次审计重置 |
| 独立日志读游标 | 16 个槽位、最大滞后 65,536 条记录 | 租约保护防止分段提前回收 |
