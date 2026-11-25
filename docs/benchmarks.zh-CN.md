# NimbleFIX 性能基准

[English](benchmarks.md) | **简体中文**

以下全部数据均在同一台机器的 Release 模式下采集，开启真实分段 mmap 日志持久化（SSE4.2 硬件 CRC32C，无内存桩存储）并通过 `-Wl,--wrap=malloc` 审计热路径堆分配。

> 相关文档：[使用手册](user-guide.zh-CN.md) · [架构设计](architecture.zh-CN.md) · [支持边界矩阵](session-support.zh-CN.md)

## 测试环境

| 项目 | 配置 |
| :--- | :--- |
| CPU | AMD Ryzen 7 7840HS（8 核 16 线程） |
| 系统 / 内核 | Linux 7.2（`x86_64`，本机 Loopback TCP 开启 `TCP_NODELAY`） |
| 编译器 | GCC 16.2.1（`xmake f -m release --tls=y --xml=y --benchmarks=y`，`-O3`） |
| 字典 | 完整标准 **FIX 4.4**（`schemas/standard/FIX44.xml`，912 字段） |
| 持久化 | 真实分段 `MAP_SHARED` mmap 日志 + SSE4.2 硬件 CRC32C（`appended` 确认级别） |
| 时钟 | 用户态 `CLOCK_MONOTONIC`（`clock_gettime`） |

> **边界说明**：Loopback TCP 测量包含完整 Linux 内核 TCP/IP 协议栈开销（`send`/`sendmsg` + `recv`）；Pre-TCP 测量隔离纯 NimbleFIX 编解码、字典校验、会话状态机与 mmap 日志开销。

---

## 1. 开放循环爆发与单向时间戳基准（`initiator-burst-bench`）

复现命令：
```sh
xmake f -m release --tls=y --xml=y --benchmarks=y -y
xmake build initiator-burst-bench
./build/linux/x86_64/release/initiator-burst-bench
```

### Part 0：消息构造 + FIX 4.4 编码
*30,000 样本（5,000 预热），交替构造市价单（`40=1`，无 `Price`）与限价单（`40=2`，带 `Price=150.25`）`NewOrderSingle`。*

| 消息 API | p50 | p99 | 堆分配（`__wrap_malloc`） |
| :--- | ---: | ---: | :---: |
| **流式编码**：`encode_fix44_stream<NewOrderSingle>` + 原生 `if` | **130.0 ns** | **170.0 ns** | **0** |
| **流式编码**：`encode_fix44_stream<NewOrderSingle>` + `std::optional` | **130.0 ns** | **180.0 ns** | **0** |
| **固定模板**：`NewOrderSingle::Fixed<128, fix::field::...>` | **220.0 ns** | **391.0 ns** | **0** |

### Part 1–3：Initiator 发送模式对比（本机 TCP + FIX 4.4 + mmap 日志）

每笔订单在 `ClOrdID (11)` 中携带 `<t_burst_ns>-<t_send_ns>`，对端在 `::recv()` 后立即打时间戳 `t_recv_ns`，报告两个单向指标：
- **CO-Free（`t_recv - t_burst_start`）**：无 Coordinated Omission 的真实延迟（从爆发信号到达至对端收到第 `i` 笔订单）。
- **Enq->Recv（`t_recv - t_send_stamp`）**：从入队/发起发送到对端收到的耗时（适合诊断传输队列，但在生产者被 I/O 阻塞时会掩盖队头阻塞）。

#### 负载 1：Ping-Pong（1 笔在途，5,000 笔，500 预热）

| 模式 | 单向 p50 | 单向 p99 | RTT p50 | 吞吐 |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`**（单线程无队列） | **4.64 µs** | **7.61 µs** | **8.61 µs** | **110,081 rt/s** |
| **2. `DirectInitiator::enqueue_stream()+flush()`**（单线程 `sendmsg`） | 4.74 µs | 8.03 µs | 8.76 µs | 102,958 rt/s |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`**（双线程 SPSC） | 5.11 µs | 7.69 µs | 8.65 µs | **112,950 rt/s** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`**（双线程 SPSC） | **5.48 µs** | **7.96 µs** | **9.92 µs** | **98,067 rt/s** |

#### 负载 2：纯密集连发 —— 0 µs 策略计算（200 批 × 32 笔，20 批预热）

| 模式 | CO-Free p50 | CO-Free p99 | Enq->Recv p50 | 吞吐 |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`**（单线程无队列） | 49.18 µs | 99.08 µs | 4.62 µs | 341,745 ord/s |
| **2. `DirectInitiator::enqueue_stream()+flush()`**（单线程 `sendmsg`） | **12.19 µs** | **19.32 µs** | 9.63 µs | **1,402,288 ord/s** |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`**（双线程 SPSC + `sendmsg`） | 13.03 µs | 25.00 µs | 12.38 µs | 1,344,907 ord/s |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`**（双线程 SPSC + `sendmsg`） | **17.18 µs** | **26.87 µs** | 12.72 µs | **1,178,200 ord/s** |

#### 负载 3：密集连发 + 每笔 2 µs 策略计算（200 批 × 32 笔）

| 模式 | CO-Free p50 | CO-Free p99 | Enq->Recv p50 | 吞吐 |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`**（单线程无队列） | 85.58 µs | 169.47 µs | 5.48 µs | 192,543 ord/s |
| **2. `DirectInitiator::enqueue_stream()+flush()`**（单线程 `sendmsg`） | 78.57 µs | 83.32 µs | 42.18 µs | 358,644 ord/s |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`**（双线程 SPSC + `sendmsg`） | **41.50 µs** | **78.99 µs** | 6.61 µs | **409,374 ord/s** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`**（双线程 SPSC + `sendmsg`） | **44.11 µs** | **81.11 µs** | **6.53 µs** | **398,843 ord/s** |

---

## 2. `DirectInitiator` 分层耗时拆解（`direct-initiator-bench`）

复现命令：
```sh
xmake build direct-initiator-bench
./build/linux/x86_64/release/direct-initiator-bench 5000 30000
```

负载：全量 912 字段 FIX 4.4 `NewOrderSingle`（7 个标量含 `Price` + 3 字段 `NoPartyIDs` 重复组，循环内现场构造）与 11 字段 `ExecutionReport`，启用分段 mmap 日志 + SSE4.2 CRC32C。

| 测量边界 | p50 | p95 | p99 | 吞吐 | 堆分配 |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **纯用户编码（`StreamWriter`）**（无会话/日志） | **140.0 ns** | 150.0 ns | **151.0 ns** | **7,089,686 msgs/s** | **0** |
| **纯用户编码（`Fixed`）**（循环内填充 + 编码，无会话/日志） | 351.0 ns | 391.0 ns | 411.0 ns | 2,804,887 msgs/s | **0** |
| **Pre-TCP 出站**（`DirectInitiator::send_stream` + mmap 日志） | **211.0 ns** | 280.0 ns | **421.0 ns** | **4,420,157 msgs/s** | **0** |
| **Pre-TCP 入站**（分帧 + 解析 + 912 字段校验 + 双日志记录 + 回调） | **721.0 ns** | 822.0 ns | **1,062.0 ns** | **1,392,046 msgs/s** | **0** |
| **Pre-TCP 往返总计**（纯 FIX 引擎往返总预算） | **942.0 ns** | 1,091.0 ns | **1,442.0 ns** | **1,058,645 rt/s** | **0** |
| **真实 TCP Loopback RTT**（内核 TCP + 全量 FIX 4.4 + mmap 日志） | **9.67 µs** | 10.67 µs | 13.28 µs | 103,837 rt/s | **0** |

---

## 3. Acceptor、`IoBackend` 与多核 `ShardedEngine`（`acceptor-bench`）

复现命令（测量 `io_uring` 需开启 `--io_uring=y`）：
```sh
xmake f -m release --tls=y --xml=y --io_uring=y --benchmarks=y -y
xmake build acceptor-bench
./build/linux/x86_64/release/acceptor-bench 2000 20000 8 all
```

### Acceptor 分层与单核 `IoBackend` 对比（`epoll` / `busy_poll` / `io_uring`）

| 层级 / `IoBackend` | 单会话 p50 / p99 | 单会话吞吐 | 8 会话 p50 / p99 | 8 会话总吞吐 |
| :--- | ---: | ---: | ---: | ---: |
| **Tier 1：Pre-TCP Acceptor**（`SessionDriver`，912 字段校验 + `send_stream` ER + mmap 日志 + flush） | **912 ns** / **1.13 µs** | **1,043,558 ord/s** | — | — |
| **Tier 2/3：`epoll`**（默认，空闲休眠） | 15.03 µs / 22.33 µs | 63,046 ord/s | 38.90 µs / 90.91 µs | 150,318 ord/s |
| **Tier 2/3：`busy_poll`**（零休眠忙轮询） | **11.86 µs** / **17.20 µs** | **76,962 ord/s** | **38.94 µs** / 94.13 µs | 149,713 ord/s |
| **Tier 2/3：`io_uring`**（Proactor `RECV`/`SEND`，空闲休眠） | 14.12 µs / 23.44 µs | 64,537 ord/s | 44.06 µs / **78.13 µs** | **166,477 ord/s**（+11%） |

### 多核 `ShardedEngine` 扩展性（`acceptor-bench 1000 10000 8/16 sharded`）
*（全量 912 字段 `FIX44.xml` 严格校验 + `NoPartyIDs[1]` + `StreamWriter` 回报 + 真实 mmap 日志，单监听端口，1-in-flight TCP Ping-Pong；同一并发会话数下各行均在同一轮测试中同口径采集。）*

| 配置 | 并发会话数 | p50 | p99 | p99.9 | 总吞吐 | 相对单核加速 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **单核 `Engine` (`epoll`)** | 8 | 39.25 µs | 92.68 µs | 181.70 µs | 148,915 ord/s | 1.00× |
| **4 分片 `ShardedEngine` (`epoll`，绑核)** | 8 | 19.14 µs | 26.28 µs | 39.48 µs | 384,232 ord/s | **2.58×**（p99 降低 **3.53×**） |
| **4 分片 `ShardedEngine` (`epoll` + `hybrid_backoff`，绑核)** | 8 | 18.93 µs | 24.81 µs | **36.31 µs** | 387,252 ord/s | **2.60×**（p99.9 降低 **5.00×**） |
| **4 分片 `ShardedEngine` (`io_uring` + `hybrid_backoff`，绑核)** | 8 | **18.20 µs** | **24.18 µs** | 37.11 µs | **401,841 ord/s** | **2.70×**（p99 降低 **3.83×**） |
| **单核 `Engine` (`epoll`)** | 16 | 88.89 µs | 196.25 µs | 400.98 µs | 149,731 ord/s | 1.00× |
| **4 分片 `ShardedEngine` (`epoll`，绑核)** | 16 | 36.72 µs | 58.93 µs | **115.40 µs** | 408,981 ord/s | **2.73×**（p99 降低 **3.33×**） |
| **4 分片 `ShardedEngine` (`epoll` + `hybrid_backoff`，绑核)** | 16 | 36.79 µs | 77.84 µs | 363.66 µs | 391,162 ord/s | **2.61×** |
| **4 分片 `ShardedEngine` (`io_uring` + `hybrid_backoff`，绑核)** | 16 | **34.51 µs** | **52.01 µs** | 121.52 µs | **430,573 ord/s** | **2.88×**（p99 降低 **3.77×**） |

> **选型建议**：`epoll` 零外部依赖且通用；`busy_poll` 适合独占核追求单会话最低延迟；`io_uring` 通过批量提交异步 `RECV`/`SEND` 显著提升多会话吞吐、降低 p99 尾延迟且空闲不空烧 CPU（配合 `IdleStrategy::hybrid_backoff` 与 `IORING_SETUP_TASKRUN_FLAG` 在多核 `ShardedEngine` 上表现最佳）。

---

## 4. NimbleFIX vs QuickFIX 1.16.0 同口径对比（`quickfix-parity-bench`）

复现命令：
```sh
git submodule update --init --recursive
xmake f -m release --tls=y --xml=y --benchmarks=y --quickfix=y -y
xmake build quickfix-parity-bench
./build/linux/x86_64/release/quickfix-parity-bench 1000 5000 schemas/standard/FIX44.xml
```

### 同口径对齐条件

| 维度 | NimbleFIX | QuickFIX 1.16.0 |
| :--- | :--- | :--- |
| **二进制 / 编译器** | 同一可执行文件，GCC 16.2.1 `-O3`（`-std=c++20`） | 同一可执行文件，GCC 16.2.1 `-O3`（`-std=c++17`） |
| **时钟与分配钩子** | `CLOCK_MONOTONIC` + 统一 `-Wl,--wrap=malloc` 与全局 `operator new/delete` | 完全一致 |
| **FIX 字典** | 全量 912 字段 `schemas/standard/FIX44.xml`，`ValidationLevel::strict` + 120 s `SendingTime` 校验 | 同一 `schemas/standard/FIX44.xml`，`UseDataDictionary=Y`，全严格校验 + `MaxLatency=120` |
| **订单与回报负载** | `NewOrderSingle`（7 标量 + `NoPartyIDs[1]`：`448`/`447`/`452`）与 11 字段 `ExecutionReport` | 字段与重复组完全一致 |
| **持久化** | 分段 `MAP_SHARED` mmap 日志 + SSE4.2 CRC32C（OS 页缓存，无 `fsync`） | `FIX::FileStore`（`PersistMessages=Y`，OS 页缓存，无 `fsync`） |
| **日志输出** | 关闭 | 关闭 |
| **TCP 对端** | 中立原始 TCP Loopback 对端（`127.0.0.1`，`TCP_NODELAY=1`） | 同一对端 |

### 实测结果（1000 预热 / 5000 样本）

| 测试项 | NimbleFIX (p50 / p99 / 堆分配) | QuickFIX 1.16.0 (p50 / p99 / 堆分配) | 加速比 (p50) |
| :--- | :--- | :--- | ---: |
| **Codec 编码**（`StreamWriter` / `Fixed`） | **150 ns** / **151 ns**（**0.00** 分配）· `Fixed` **331 ns** | 1,312 ns / 1,383 ns / 19.00 | **8.75×** / **3.96×** |
| **解析 + 全量 `FIX44.xml` 严格校验 + 读取** | **361 ns** / **371 ns** / **0.00** | 2,464 ns / 2,495 ns / 15.00 | **6.83×** |
| **TCP Initiator Ping-Pong RTT**（`DirectInitiator`） | **8.85 µs** / **11.89 µs** / **0.00**（110k rt/s） | 21.58 µs / 29.90 µs / 35.06（45k rt/s） | **2.44×** |
| **TCP Initiator Ping-Pong RTT**（`Engine busy_poll` / `epoll`） | **10.52 µs** / **16.66 µs** / **0.00** | 21.58 µs / 29.90 µs / 35.06 | **2.05×** / **1.30×** |
| **TCP Acceptor Turnaround RTT**（`Engine busy_poll` / `epoll`） | **9.45 µs** / **11.46 µs**（p99 **12.96 µs**，**0.00**） | 21.20 µs / 34.48 µs / 37.04（44k ord/s） | **2.24×** / **1.85×** |
| **TCP 32 笔密集连发**（`enqueue_stream` + `sendmsg`，CO-Free 单向） | **13.95 µs** / **35.88 µs** / **0.00**（**1.23M/s**） | 178.67 µs / 352.25 µs / 28.06（92k/s） | **12.81×**（吞吐 **13.3×**） |
