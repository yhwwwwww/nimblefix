# NimbleFIX 架构设计

[English](architecture.md) | **简体中文**

NimbleFIX 采用三层模块化 C++20 架构，围绕确定性的亚微秒级热路径、稳态零堆分配以及严格的 FIX 协议与崩溃恢复保证进行设计。

> 相关文档：[使用手册](user-guide.zh-CN.md) · [支持边界矩阵](session-support.zh-CN.md) · [性能基准](benchmarks.zh-CN.md)

---

## 1. 三层模块化架构

```text
+-----------------------------------------------------------------------------+
| Full Runtime: 多会话 Engine、多核 ShardedEngine、Acceptor、Inline/Queued     |
|               工作池、可插拔 IoBackend（epoll / busy_poll / io_uring）、     |
|               MPSC Sender、SPSC ProducerLane、会话窗口调度                   |
|               (NimbleFIX::nimblefix)                                        |
+-----------------------------------------------------------------------------+
| Core + TLS:   DirectTlsTransport（OpenSSL 3、TLS 1.2/1.3、CA/SAN、mTLS）     |
|               (NimbleFIX::nimblefix_core_tls)                               |
+-----------------------------------------------------------------------------+
| Core:         流式编码器（StreamWriter / Fixed）、编译期生成字典、           |
|               分段 mmap 日志（SSE4.2 CRC32C、跨进程 SPMC JournalTailer）、   |
|               SessionCore、TcpTransport（write_batch/sendmsg）、             |
|               DirectInitiator、ConnectionStrategy 主备切换、                 |
|               DirectSendQueue（SPSC）、旁路 mmap 遥测、JournalReplayer       |
|               (NimbleFIX::nimblefix_core — 仅 C++20 + pthread)              |
+-----------------------------------------------------------------------------+
```

- **Core（`nimblefix_core`）**：面向单会话交易客户端，直接提供协议状态机、mmap 日志与非阻塞 TCP，无 `Engine`、`epoll`、互斥锁或后台线程开销（Release 二进制约 336 KB）。
- **Core + TLS（`nimblefix_core_tls`）**：仅需替换 `DirectInitiator` 的 `Transport` 模板参数即可启用非阻塞 OpenSSL 3 TLS/mTLS。
- **Full Runtime（`nimblefix`）**：提供多会话复用、Acceptor 监听、可插拔 `IoBackend`、`Queued` 工作线程池、MPSC/SPSC 跨线程发送以及多核 `ShardedEngine`。

---

## 2. 编解码器与消息构造

### 流式编码器（`StreamWriter`）
默认出站 API 将强类型 FIX 字段直接流式写入目标报文缓冲区（在 `DirectInitiator` 中直接写入 mmap 日志的 `reserve()` 槽位），无中间消息对象、零堆分配：
1. **静态会话前缀与动态标准头分离**：会话生命周期内不变的字段（`8=<BeginString>\x019=` 与 `49=<SenderCompID>\x0156=<TargetCompID>\x01`）缓存于 `StaticSessionPrefix`；每笔可动态变化的头字段（`50=SenderSubID`、`57=TargetSubID`、`142=SenderLocationID`、`115/116=OnBehalfOf*`、`128/129=DeliverTo*` 等）可在正文前通过 `w.header()` 写入。
2. **内联格式化与校验**：每个字段写入函数直接写入编译期 Tag 前缀（`"11="`、`"44="` 等）与格式化值，内联完成枚举/时间戳/文本校验与必填位掩码检查，原生支持 `if`/`for`、`std::optional<T>`、自动 `DATA`/`LENGTH` 配对与嵌套重复组。
3. **单遍回填 `BodyLength` 与 `CheckSum`**：一次连续扫描完成精确 `BodyLength`（Tag 9）回填与 SIMD 友好的 `CheckSum`（Tag 10）计算。

| API | 可变性与控制流 | 内存分配 | 复用方式 | 主要用途 |
| :--- | :--- | :--- | :--- | :--- |
| **流式编码（默认）**：`send_stream<Msg>(fn)` / `encode_stream<Msg>` | 原生 `if`/`for`、`std::optional<T>`、嵌套重复组、`w.header()` | 0 堆分配 | 无状态 lambda | 最快、最灵活的生产主力接口（~110–150 ns） |
| **固定模板**：`NewOrderSingle::Fixed<ByteCap, fix::field::...>` | 固定字段集合、存在位图 | 0 堆分配（栈缓冲） | `reset()` 原地复用 | 可反复原地改写的预填充模板消息 |
| **无字典 / 动态**：`nf::message<"D">()` / `DynamicMessage` | 任意运行期 `(tag, value)` 对 | SSO / 堆 | 可变 | 底层管理帧与运行期自定义 Tag |

### 紧凑成员索引与子集裁剪
- `nimblefix-schema-gen` 将标准或自定义 FIX XML 字典编译为紧凑 `constexpr` 元数据表与基于 `switch(tag)` 的单遍校验器，并配合代际戳 O(1) `ParsedFieldIndex` 加速强类型字段读取。
- 所有每消息槽位表与去重位图均按紧凑成员下标索引，不按原始 Tag 号建表，原生支持 `1..999,999,999` 范围内的任意自定义 Tag 且无内存膨胀。
- 支持 `--messages` 与 `--from-fix-log` 裁剪未使用的业务消息及可选字段，自动保留必需的管理消息、组件与重复组。

---

## 3. 分段 mmap 日志与崩溃恢复

每个会话均使用预分配的分段 `MAP_SHARED` mmap 日志：

- **零拷贝出站路径**：`journal.reserve()` 将映射段指针直接交给编码器；编码完成后 `journal.commit()` 写入硬件 SSE4.2 CRC32C 并通过 release-store 发布提交字，socket 直接从已提交字节发送。
- **严格落盘顺序**：
  - **出站**：提交至日志 -> 写入 socket。
  - **入站**：提交报文与下一期望序列号 -> 调用业务回调 -> 提交投递完成标记（推进 FIX Tag `369` `LastMsgSeqNumProcessed`）。
- **崩溃恢复**：同机进程崩溃或 `SIGKILL` 重启后，重新打开日志自动重建序列号状态，重投已落盘但未完成处理的入站消息（`DeliveryInfo::is_recovery == true`），复用已落盘的关联回复，并从保留段准确响应 `ResendRequest`、`GapFill` 与业务重传。
- **热路径外维护**：新分段预分配（`.tmp` + 原子 `rename`）与旧分段回收由 `poll_maintenance()` 在热路径外执行，避免发送路径触发 `open`/`mmap`/`unlink` 系统调用。
- **持久性边界**：确认级别 `appended` 保障宿主机与文件系统健康前提下的进程崩溃恢复；热路径不执行逐消息 `fsync`/`msync`。

---

## 4. `DirectInitiator` 的三种发送模式

`DirectInitiator<Schema, Handler, Transport>` 在同一会话与日志上提供三种可组合发送路径：

1. **单线程直发（`send_stream`）**：单线程无队列，直接编码进 mmap 日志并执行一次非阻塞 `::send()`，单笔/Ping-Pong 延迟最低。
2. **同线程攒批（`enqueue_stream` + `flush`）**：单线程合并，最多连续暂存 64 笔帧到日志，再通过一次 `sendmsg()` 聚合发送。
3. **双线程无锁 SPSC 流水线（`DirectSendQueue` + `drain`）**：策略线程通过 `queue.try_push_stream(...)` 将正文写入缓存行对齐的无锁环形缓冲（0 锁 / 0 `malloc`）；独立 I/O 线程调用 `init.drain(queue)` 批量出队最多 64 笔、补齐 `MsgSeqNum`/`SendingTime` 落盘并通过单次 `sendmsg()` 发出。

---

## 5. `Engine`、可插拔 `IoBackend` 与多核 `ShardedEngine`

### 单写者协议归属
`Engine<Schema, Policy>` 在唯一一个协议归属线程（`engine.run()` 或调用方驱动的 `start()`/`poll_once()`）上运行全部协议状态、TCP/TLS I/O、定时器与日志写入：
- 每个会话严格只有一个协议写者，`SessionCore`、序列号与 mmap 日志提交全程无锁。
- 业务回调支持 `Inline`（在协议线程内联执行，延迟最低）或 `Queued`（派发至有界工作线程池，单会话同时最多 1 个在途回调以保持顺序）。
- 外部线程可通过有界 MPSC `Sender` 或无锁 SPSC `ProducerLane` 提交出站消息。

### 可插拔 `IoBackend`
操作系统 I/O 多路复用层抽象为 `IoBackend`（`<nimblefix/io_backend.hpp>`）：
- **`epoll`（默认）**：通用就绪事件循环，无额外依赖。
- **`busy_poll`**：零休眠非阻塞轮询，适合独占隔离核追求最低单会话延迟。
- **`io_uring`（可选 Linux Proactor）**：基于 `liburing`（`IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_R_DISABLED`），批量提交异步 `IORING_OP_RECV` / `IORING_OP_SEND`，空闲时干净休眠。

### 多核 `ShardedEngine`
面向多核高并发场景（`<nimblefix/sharded_engine.hpp>`）：
- 运行 `K` 个完全独立的 `IoBackend` Reactor 线程，支持按分片绑定 Linux CPU 核（`cpu_cores` / `pin_current_thread`）及可配置 `IdleStrategy`（`sleep`、`busy_spin`、`hybrid_backoff`）。
- 每个会话静态固定到一个分片，分片内严格保持单写者无锁 `SessionCore` 与 mmap 日志归属。
- 共享 Acceptor 监听器在 Shard 0 绑定一次；客户端连接完成 `Logon (35=A)` 路由后，socket 直接移交给所属分片的 Reactor 线程处理。

---

## 6. 旁路 mmap 遥测与历史回放

- **旁路 mmap 遥测（`<nimblefix/telemetry.hpp>`）**：可选 `MAP_SHARED` `.nfxt` 共享内存区域，每个会话拥有独立缓存行对齐槽位，仅由协议归属线程使用 relaxed load+store 写入（零锁、零堆分配、零系统调用）。开启 `telemetry_stage_probes = true` 后，为 `read`、`parse`、`validate`、`journal`、`write` 五个阶段记录 433 桶对数线性纳秒级 HDR 直方图；`TelemetryReader` 在外部进程以 `O_RDONLY`/`PROT_READ` 只读映射并导出结构化快照或 Prometheus 指标。
- **确定性历史回放与跨进程 SPMC 追尾（`<nimblefix/journal.hpp>`、`<nimblefix/replay.hpp>`）**：`JournalTailer` 以 `O_RDONLY`/`PROT_READ` 只读映射在线或离线 `.nfxj` 分段（不抢占 `LOCK_EX`），通过 acquire-load 无锁感知写端 release 发布的 `record_commit_marker` 并自动跨段追尾，在不影响交易写端热路径的前提下支持 DropCopy、合规审计与风控旁路进程。`JournalReplayer` 按路径回放时默认基于 `JournalTailer`（同时提供进程内带租约 `JournalReader` 重载），支持方向（`inbound`/`outbound`/`both`）、序列号区间、Epoch、管理帧与 `WireHints` 过滤，并按全速、原始 `52=SendingTime` 或倍速回放至回调或 TCP 端点。
