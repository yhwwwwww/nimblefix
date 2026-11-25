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
| **1. `DirectInitiator::send_stream()`**（单线程无队列） | **4.69 µs** | **7.73 µs** | **8.86 µs** | **105,905 rt/s** |
| **2. `DirectInitiator::enqueue_stream()+flush()`**（单线程 `sendmsg`） | 5.42 µs | 9.39 µs | 10.39 µs | 88,421 rt/s |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`**（双线程 SPSC） | 5.27 µs | 8.72 µs | 9.09 µs | 101,836 rt/s |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`**（双线程 SPSC） | 7.69 µs | 12.07 µs | 12.27 µs | 74,338 rt/s |

#### 负载 2：纯密集连发 —— 0 µs 策略计算（200 批 × 32 笔，20 批预热）

| 模式 | CO-Free p50 | CO-Free p99 | Enq->Recv p50 | 吞吐 |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`**（单线程无队列） | 49.93 µs | 98.19 µs | 4.60 µs | 335,194 ord/s |
| **2. `DirectInitiator::enqueue_stream()+flush()`**（单线程 `sendmsg`） | 14.58 µs | **19.83 µs** | 11.26 µs | 1,274,415 ord/s |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`**（双线程 SPSC + `sendmsg`） | **12.86 µs** | 22.64 µs | 12.16 µs | **1,430,830 ord/s** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`**（双线程 SPSC） | 67.28 µs | 117.28 µs | 53.81 µs | 277,608 ord/s |

#### 负载 3：密集连发 + 每笔 2 µs 策略计算（200 批 × 32 笔）

| 模式 | CO-Free p50 | CO-Free p99 | Enq->Recv p50 | 吞吐 |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`**（单线程无队列） | 86.87 µs | 228.77 µs | 5.55 µs | 179,146 ord/s |
| **2. `DirectInitiator::enqueue_stream()+flush()`**（单线程 `sendmsg`） | 80.85 µs | 217.01 µs | 45.87 µs | 320,174 ord/s |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`**（双线程 SPSC + `sendmsg`） | **42.62 µs** | **76.08 µs** | 7.42 µs | **421,837 ord/s** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`**（双线程 SPSC） | 64.77 µs | 118.97 µs | 19.07 µs | 280,372 ord/s |

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

### Acceptor 分层（`epoll`）
| 层级 | p50 | p95 | p99 | 吞吐 |
| :--- | ---: | ---: | ---: | ---: |
| **Tier 1：Pre-TCP Acceptor**（收帧 -> 912 字段校验 -> 回调内回 ER -> mmap 日志 -> flush） | **1.71 µs** | 1.83 µs | 3.03 µs | **581,156 ord/s** |
| **Tier 2：单会话 TCP**（`Engine<fix44::Schema, Inline>`，1 个 `epoll` 线程） | **14.68 µs** | 18.59 µs | 32.07 µs | **62,553 ord/s** |
| **Tier 3：8 并发会话 TCP**（`Engine<fix44::Schema, Inline>`，1 个 `epoll` 线程） | **44.56 µs** | 94.84 µs | 114.46 µs | **133,972 ord/s** |

### 同进程 `IoBackend` 对比（`acceptor-bench 2000 20000 8 all`）

| `IoBackend` | 单会话 p50 | 单会话 p99 | 单会话吞吐 | 8 会话 p50 | 8 会话 p99 | 8 会话总吞吐 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **`epoll`**（默认） | 14.68 µs | 32.07 µs | 62,553 ord/s | **44.56 µs** | 114.46 µs | 133,972 ord/s |
| **`busy_poll`**（零休眠轮询） | **12.45 µs** | 23.38 µs | **73,464 ord/s** | 45.38 µs | 118.44 µs | 129,649 ord/s |
| **`io_uring`**（Proactor `RECV`/`SEND`，空闲休眠） | 14.58 µs | **19.56 µs** | 66,515 ord/s | 49.10 µs | **97.90 µs** | **146,037 ord/s**（+9%） |

### 多核 `ShardedEngine` 扩展性（`acceptor-bench 1000 10000 8/16 sharded`）
*（全量 912 字段 `FIX44.xml` 严格校验 + `NoPartyIDs[1]` + `StreamWriter` 回报 + 真实 mmap 日志，单监听端口，1-in-flight TCP Ping-Pong。）*

| 配置 | 并发会话数 | p50 | p99 | p99.9 | 总吞吐 | 相对单核加速 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **单核 `Engine` (`epoll`)** | 8 | 32.99 µs | 76.91 µs | 227.25 µs | 176,387 ord/s | 1.00× |
| **4 分片 `ShardedEngine` (`epoll`，绑核)** | 8 | 16.58 µs | 22.15 µs | 31.21 µs | 433,318 ord/s | **2.46×**（p99 降低 **3.47×**） |
| **4 分片 `ShardedEngine` (`hybrid_backoff`，绑核)** | 8 | **16.39 µs** | **21.27 µs** | **28.98 µs** | **443,302 ord/s** | **2.51×**（p99.9 降低 **7.84×**） |
| **单核 `Engine` (`epoll`)** | 16 | 75.64 µs | 161.11 µs | 346.42 µs | 176,184 ord/s | 1.00× |
| **4 分片 `ShardedEngine` (`epoll`，绑核)** | 16 | 31.41 µs | 51.06 µs | 112.79 µs | 466,617 ord/s | **2.65×**（p99 降低 **3.15×**） |
| **4 分片 `ShardedEngine` (`hybrid_backoff`，绑核)** | 16 | **31.19 µs** | **49.99 µs** | **96.74 µs** | **470,973 ord/s** | **2.67×**（p99 降低 **3.22×**） |

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
| **Codec 编码**（`StreamWriter` / `Fixed`） | **170 ns** / **181 ns**（**0.00** 分配）· `Fixed` **371 ns** | 1,463 ns / 1,523 ns / 19.00 | **8.61×** / **3.94×** |
| **解析 + 全量 `FIX44.xml` 严格校验 + 读取** | **421 ns** / **431 ns** / **0.00** | 2,795 ns / 3,386 ns / 15.00 | **6.64×** |
| **TCP Initiator Ping-Pong RTT**（`DirectInitiator`） | **10.21 µs** / **17.12 µs** / **0.00**（92k rt/s） | 23.72 µs / 34.56 µs / 35.06（41k rt/s） | **2.32×** |
| **TCP Initiator Ping-Pong RTT**（`Engine busy_poll` / `epoll`） | **14.39 µs** / **19.16 µs** / **0.00** | 23.72 µs / 34.56 µs / 35.06 | **1.65×** / **1.24×** |
| **TCP Acceptor Turnaround RTT**（`Engine busy_poll` / `epoll`） | **10.59 µs** / **11.57 µs**（p99 **14.33 µs**，**0.00**） | 23.20 µs / 33.92 µs / 37.00（42k ord/s） | **2.19×** / **2.01×** |
| **TCP 32 笔密集连发**（`enqueue_stream` + `sendmsg`，CO-Free 单向） | **12.63 µs** / **23.51 µs** / **0.00**（**1.34M/s**） | 198.47 µs / 399.32 µs / 28.06（80k/s） | **15.71×**（吞吐 **16.6×**） |
