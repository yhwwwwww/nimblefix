# NimbleFIX 性能基准

[English](benchmarks.md) | **简体中文**

以下全部数据均在同一台机器的 Release 模式下采集，开启真实分段 mmap 日志持久化（SSE4.2 硬件 CRC32C，无内存桩存储）并通过统一的线程安全分配审计器（`-Wl,--wrap={malloc,calloc,realloc,aligned_alloc,posix_memalign}` 与全局 `operator new/delete`，含 C++17 `std::align_val_t` 重载）审计热路径堆分配。

> 相关文档：[使用手册](user-guide.zh-CN.md) · [架构设计](architecture.zh-CN.md) · [支持边界矩阵](session-support.zh-CN.md)

## 测试环境

| 项目 | 配置 |
| :--- | :--- |
| CPU | AMD Ryzen 7 7840HS（8 核 16 线程） |
| 系统 / 内核 | Linux 7.2（`x86_64`，本机 Loopback TCP 开启 `TCP_NODELAY`） |
| 编译器 | GCC 16.2.1（`cmake -DCMAKE_BUILD_TYPE=Release`，`-O3`） |
| 字典 | 完整标准 **FIX 4.4**（`schemas/standard/FIX44.xml`，912 字段，覆盖 Header/Trailer/Body 严格校验） |
| 持久化 | 真实分段 `MAP_SHARED` mmap 日志 + SSE4.2 硬件 CRC32C（`appended` 确认级别） |
| 时钟 | 用户态 `CLOCK_MONOTONIC`（`clock_gettime`） |
| 分配审计口径 | 共享 `bench/alloc_audit.hpp`：原子计数拦截 `malloc`/`calloc`/`realloc`/`aligned_alloc`/`posix_memalign` 与全局 `operator new/delete`（覆盖可执行文件及所有静态链接库，含 NimbleFIX 与内嵌 QuickFIX；动态链接系统库内部未经过全局 `operator new` 的分配不在 `--wrap` 覆盖范围内） |

> **边界说明**：Loopback TCP 测量包含完整 Linux 内核 TCP/IP 协议栈开销（`send`/`sendmsg` + `recv`）；Pre-TCP 测量隔离纯 NimbleFIX 编解码、字典校验、会话状态机与 mmap 日志开销。在长周期多段 Acceptor 测试（≥20,000 笔）中，每次跨越 4 MiB 日志分段边界会触发一次原子 `.ckp` 检查点写盘（每 4 MiB 分段约 4 次小分配，折合 `0.0004 alloc/order`）；稳态每笔消息热路径本身仍为 **0** 次堆分配。

---

## 1. 爆发、开放循环定时调度与单向时间戳基准（`initiator-burst-bench`）

复现命令：
```sh
cmake --build build-cmake --target initiator-burst-bench -j
./build-cmake/initiator-burst-bench
```

### Part 0：消息构造 + FIX 4.4 编码
*30,000 样本（5,000 预热），交替构造市价单（`40=1`，无 `Price`）与限价单（`40=2`，带 `Price=150.25`）`NewOrderSingle`。*

| 消息 API | p50 | p99 | 堆分配 |
| :--- | ---: | ---: | :---: |
| **流式编码**：`encode_fix44_stream<NewOrderSingle>` + 原生 `if` | **121.0 ns** | **131.0 ns** | **0** |
| **流式编码**：`encode_fix44_stream<NewOrderSingle>` + `std::optional` | **121.0 ns** | **150.0 ns** | **0** |
| **固定模板**：`NewOrderSingle::Fixed<128, fix::field::...>` | **211.0 ns** | **250.0 ns** | **0** |

### 负载 1–4：Initiator 发送模式对比（本机 TCP + FIX 4.4 + mmap 日志）

每笔订单在 `ClOrdID (11)` 中携带 `<t_burst_ns>-<t_send_ns>`，对端在 `::recv()` 后立即打时间戳 `t_recv_ns`，报告三类指标：
- **Burst->Recv（`t_recv - t_burst_start`）** *（负载 2、3）*：单批同时到达爆发场景下，从批次开始时刻至对端收到第 `i` 笔订单的单向耗时（批次间等待对端收完以隔离各批）。
- **Enq->Recv（`t_recv - t_send_stamp`）**：从入队/发起发送到对端收到的耗时（适合诊断传输队列，但在生产者被 I/O 阻塞时会掩盖队头阻塞）。
- **OpenLoop（`t_recv - t_sched`）** *（负载 4）*：由独立外部时间表（`t_sched = t_origin + b * 500 µs`，**批次间不等待排空**）驱动的真正无 Coordinated Omission 开放循环延迟，完整计入批内排队与跨批积压传导。

#### 负载 1：Ping-Pong（1 笔在途，5,000 笔，500 预热）

| 模式 | 单向 p50 | 单向 p99 | RTT p50 / p99 | 吞吐 | 堆分配 |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **1. `DirectInitiator::send_stream()`**（单线程无队列） | **4.62 µs** | 7.04 µs | **8.58 µs** / 12.57 µs | 107,695 rt/s | **0** |
| **2. `DirectInitiator::enqueue_stream()+flush()`**（单线程 `sendmsg`） | 4.65 µs | 7.60 µs | 8.59 µs / 13.01 µs | 111,536 rt/s | **0** |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`**（双线程 SPSC） | 5.05 µs | **6.89 µs** | **8.58 µs** / **11.25 µs** | **114,535 rt/s** | **0** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`**（双线程 SPSC） | 5.48 µs | 8.51 µs | 9.82 µs / 14.54 µs | 96,547 rt/s | **0** |

#### 负载 2：单批同时到达爆发 —— 0 µs 策略计算（200 批 × 32 笔，批间排空）

| 模式 | Burst->Recv p50 | Burst->Recv p99 | Enq->Recv p50 | 吞吐 | 堆分配 |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **1. `DirectInitiator::send_stream()`**（单线程无队列） | 49.30 µs | 95.30 µs | 4.87 µs | 344,234 ord/s | **0** |
| **2. `DirectInitiator::enqueue_stream()+flush()`**（单线程 `sendmsg`） | **12.56 µs** | **17.05 µs** | 9.84 µs | **1,435,952 ord/s** | **0** |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`**（双线程 SPSC + `sendmsg`） | 12.93 µs | 19.27 µs | 12.27 µs | 1,394,570 ord/s | **0** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`**（双线程 SPSC + `sendmsg`） | 16.94 µs | 23.23 µs | 12.57 µs | 1,275,895 ord/s | **0** |

#### 负载 3：单批同时到达爆发 + 每笔 2 µs 策略计算（200 批 × 32 笔，批间排空）

| 模式 | Burst->Recv p50 | Burst->Recv p99 | Enq->Recv p50 | 吞吐 | 堆分配 |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **1. `DirectInitiator::send_stream()`**（单线程无队列） | 87.51 µs | 191.76 µs | 5.51 µs | 188,951 ord/s | **0** |
| **2. `DirectInitiator::enqueue_stream()+flush()`**（单线程 `sendmsg`） | 78.31 µs | 83.39 µs | 42.18 µs | 361,942 ord/s | **0** |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`**（双线程 SPSC + `sendmsg`） | **41.34 µs** | **75.53 µs** | 6.49 µs | **427,094 ord/s** | **0** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`**（双线程 SPSC + `sendmsg`） | 43.83 µs | 81.11 µs | **6.35 µs** | 398,380 ord/s | **0** |

#### 负载 4：开放循环定时突发（200 批 × 32 笔 @ 每批 500 µs = 64,000 目标 ord/s，批间不等待）

| 模式 | OpenLoop p50 | OpenLoop p99 | OpenLoop max | 实际速率 | 背压重试 | 堆分配 |
| :--- | ---: | ---: | ---: | ---: | :---: | :---: |
| **1. `DirectInitiator::send_stream()`**（单线程无队列） | 49.59 µs | 96.40 µs | 113.99 µs | 64,261 ord/s | 0 | **0** |
| **2. `DirectInitiator::enqueue_stream()+flush()`**（单线程 `sendmsg`） | **14.95 µs** | **18.89 µs** | **20.00 µs** | 64,303 ord/s | 0 | **0** |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`**（双线程 SPSC + `sendmsg`） | 15.86 µs | 24.11 µs | 85.69 µs | 64,302 ord/s | 0 | **0** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`**（双线程 SPSC + `sendmsg`） | 17.67 µs | 40.04 µs | 48.84 µs | 64,302 ord/s | 0 | **0** |

---

## 2. `DirectInitiator` 分层耗时拆解（`direct-initiator-bench`）

复现命令：
```sh
cmake --build build-cmake --target direct-initiator-bench -j
./build-cmake/direct-initiator-bench 5000 30000
```

负载：全量 912 字段 FIX 4.4 `NewOrderSingle`（7 个标量含 `Price` + `NoPartyIDs[1]`，含 3 个参与方字段 `448`/`447`/`452`，循环内现场构造）与 11 字段 `ExecutionReport`，启用分段 mmap 日志 + SSE4.2 CRC32C。

| 测量边界 | p50 | p95 | p99 | 吞吐 | 堆分配 |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **纯用户编码（`StreamWriter`）**（无会话/日志） | **110.0 ns** | 111.0 ns | **120.0 ns** | **9,066,848 msgs/s** | **0** |
| **纯用户编码（`Fixed`）**（循环内填充 + 编码，无会话/日志） | 300.0 ns | 431.0 ns | 481.0 ns | 3,217,353 msgs/s | **0** |
| **Pre-TCP 出站**（`DirectInitiator::send_stream` + mmap 日志） | **220.0 ns** | 241.0 ns | **341.0 ns** | **4,432,308 msgs/s** | **0** |
| **Pre-TCP 入站**（分帧 + 解析 + 912 字段校验 + 双日志记录 + 回调） | **662.0 ns** | 701.0 ns | **941.0 ns** | **1,466,009 msgs/s** | **0** |
| **Pre-TCP 往返总计**（纯 FIX 引擎往返总预算） | **881.0 ns** | 941.0 ns | **1,252.0 ns** | **1,101,637 rt/s** | **0** |
| **真实 TCP Loopback RTT**（内核 TCP + 全量 FIX 4.4 + mmap 日志） | **8.63 µs** | 10.57 µs | **12.20 µs** | **111,166 rt/s** | **0** |

---

## 3. Acceptor、`IoBackend` 与多核 `ShardedEngine`（`acceptor-bench`）

复现命令（测量 `io_uring` 需开启 `-DNIMBLEFIX_ENABLE_IO_URING=ON` 或 `--io_uring=y`）：
```sh
cmake -S . -B build-uring -DCMAKE_BUILD_TYPE=Release -DNIMBLEFIX_ENABLE_IO_URING=ON \
  -DNIMBLEFIX_BUILD_STANDARD_SCHEMAS=ON -DNIMBLEFIX_BUILD_BENCHMARKS=ON
cmake --build build-uring --target acceptor-bench -j
./build-uring/acceptor-bench 2000 20000 8 all
./build-uring/acceptor-bench 1000 10000 8 sharded
./build-uring/acceptor-bench 1000 10000 16 sharded
```

### Acceptor 分层与单核 `IoBackend` 对比（`epoll` / `busy_poll` / `io_uring`）

| 层级 / `IoBackend` | 单会话 p50 / p99 | 单会话吞吐（20k 笔堆分配） | 8 会话 p50 / p99 | 8 会话总吞吐（80k 笔堆分配） |
| :--- | ---: | ---: | ---: | ---: |
| **Tier 1：Pre-TCP Acceptor**（`SessionDriver`，912 字段校验 + `send_stream` ER + mmap 日志 + flush） | **842 ns** / **1.02 µs** *（冷机 `epoll` 构建）* · 962 ns（`all` 同轮） | **1,123,600 ord/s**（8*） | — | — |
| **Tier 2/3：`epoll`**（默认，空闲休眠） | 10.88 µs / 14.54 µs *（冷机）* · 14.95 µs（`all`） | 85,888 ord/s（8*） | 29.68 µs / 69.18 µs *（冷机）* · 38.39 µs（`all`） | 200,699 ord/s *（冷机）* · 156,313 ord/s（`all`，32–36*） |
| **Tier 2/3：`busy_poll`**（零休眠忙轮询） | **11.56 µs** / **17.75 µs**（`all`） | **81,635 ord/s**（8*） | **37.96 µs** / 93.40 µs（`all`） | 159,385 ord/s（`all`，36*） |
| **Tier 2/3：`io_uring`**（两阶段 Proactor `RECV`/`SEND`，空闲休眠） | 14.00 µs / 21.94 µs（`all`） | 65,621 ord/s（8*） | 45.65 µs / **83.71 µs**（`all`） | **164,184 ord/s**（`all`，32*） |

*\* 全稳态窗口实测：单笔消息路径 0 分配；20k / 80k 笔中出现的 8 / 32–36 次分配（每跨 4 MiB 日志分段约 4 次，折合 `0.0004/order`）均来自周期性原子 `.ckp` 检查点写盘与分段维护。*

### 多核 `ShardedEngine` 扩展性（`acceptor-bench 1000 10000 8/16 sharded`）
*（全量 912 字段 `FIX44.xml` 严格校验 + `NoPartyIDs[1]` + `StreamWriter` 回报 + 真实 mmap 日志，单监听端口，1-in-flight TCP Ping-Pong；同一并发会话数下各行均在同一轮测试中同口径采集。）*

| 配置 | 并发会话数 | p50 | p99 | p99.9 | 总吞吐 | 相对单核加速 | 总堆分配* |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| **单核 `Engine` (`epoll`)** | 8 | 39.79 µs | 113.57 µs | 357.34 µs | 150,951 ord/s | 1.00× | 36 |
| **4 分片 `ShardedEngine` (`epoll`，绑核)** | 8 | 19.32 µs | 30.63 µs | 64.06 µs | 383,488 ord/s | **2.54×**（p99 降低 **3.71×**） | 32 |
| **4 分片 `ShardedEngine` (`epoll` + `hybrid_backoff`，绑核)** | 8 | 18.97 µs | 25.76 µs | **43.29 µs** | 396,827 ord/s | **2.63×**（p99 降低 **4.41×**） | 32 |
| **4 分片 `ShardedEngine` (`io_uring` + `hybrid_backoff`，绑核)** | 8 | **18.31 µs** | **25.45 µs** | 44.36 µs | **407,095 ord/s** | **2.70×**（p99 降低 **4.46×**） | 32 |
| **单核 `Engine` (`epoll`)** | 16 | 89.97 µs | 214.17 µs | 338.46 µs | 148,186 ord/s | 1.00× | 64 |
| **4 分片 `ShardedEngine` (`epoll`，绑核)** | 16 | 36.60 µs | 59.00 µs | 105.12 µs | 425,861 ord/s | **2.87×**（p99 降低 **3.63×**） | 64 |
| **4 分片 `ShardedEngine` (`epoll` + `hybrid_backoff`，绑核)** | 16 | 36.19 µs | 57.93 µs | **89.31 µs** | 428,623 ord/s | **2.89×**（p99 降低 **3.70×**） | 68 |
| **4 分片 `ShardedEngine` (`io_uring` + `hybrid_backoff`，绑核)** | 16 | **34.77 µs** | **52.17 µs** | 99.78 µs | **446,798 ord/s** | **3.02×**（p99 降低 **4.11×**） | 64 |

*\* 覆盖 80,000 笔（8 会话）/ 160,000 笔（16 会话）：折合 `0.0004 alloc/order`（每会话 1 次 4 MiB 分段 checkpoint 轮转），热路径单笔 0 分配。*

> **选型建议**：`epoll` 零外部依赖且通用；`busy_poll` 适合独占核追求单会话最低延迟；`io_uring` 通过两阶段异步 `RECV`/`SEND`（`stage_tx` -> `commit_tx_written`）批量提交系统调用，在不空烧 CPU 的前提下显著提升多会话吞吐、降低 p99 尾延迟（配合 `IdleStrategy::hybrid_backoff` 与 `IORING_SETUP_TASKRUN_FLAG` 在多核 `ShardedEngine` 上表现最佳）。

---

## 4. NimbleFIX vs QuickFIX 1.16.0 同口径对比（`quickfix-parity-bench`）

复现命令：
```sh
git submodule update --init --recursive
cmake --build build-cmake --target quickfix-parity-bench -j
./build-cmake/quickfix-parity-bench 1000 5000 schemas/standard/FIX44.xml
```

### 同口径对齐条件

| 维度 | NimbleFIX | QuickFIX 1.16.0 |
| :--- | :--- | :--- |
| **二进制 / 编译器** | 同一可执行文件，GCC 16.2.1 `-O3`（`-std=c++20`） | 同一可执行文件，GCC 16.2.1 `-O3`（`-std=c++17`） |
| **时钟与分配钩子** | `CLOCK_MONOTONIC` + 统一 `-Wl,--wrap={malloc,calloc,realloc,aligned_alloc,posix_memalign}` 与全局 `operator new/delete` | 完全一致 |
| **FIX 字典与一致性门禁** | 全量 912 字段 `schemas/standard/FIX44.xml`，`ValidationLevel::strict`（Header + Trailer + Body + 分区顺序，计时前执行 12 组通过/拒绝一致性门禁）+ `check_sending_time = true`（`DirectInitiator` 与 `Engine` 均启用 120 s 偏差检查） | 同一 `schemas/standard/FIX44.xml`，`UseDataDictionary=Y`，全严格校验 + `CheckLatency=Y`、`MaxLatency=120` |
| **订单与回报负载** | `NewOrderSingle`（7 标量 + `NoPartyIDs[1]`：`448`/`447`/`452`）与 11 字段 `ExecutionReport` | 字段与重复组完全一致 |
| **持久化** | 分段 `MAP_SHARED` mmap 日志 + SSE4.2 CRC32C（OS 页缓存，无 `fsync`） | `FIX::FileStore`（`PersistMessages=Y`，OS 页缓存，无 `fsync`） |
| **日志输出** | 关闭 | 关闭 |
| **TCP 对端** | 中立原始 TCP Loopback 对端（`127.0.0.1`，`TCP_NODELAY=1`） | 同一对端 |

### 实测结果（1000 预热 / 5000 样本）

| 测试项 | NimbleFIX (p50 / p99 / 堆分配) | QuickFIX 1.16.0 (p50 / p99 / 堆分配) | 加速比 (p50) |
| :--- | :--- | :--- | ---: |
| **Suite 1：Codec 编码**（`StreamWriter`） | **150 ns** / **161 ns** / **0.00**（6.52M/s） | 1,172 ns / 1,222 ns / 19.00（849k/s） | **7.81×** |
| **Suite 1：Codec 编码**（`Fixed`） | **301 ns** / **531 ns** / **0.00**（3.07M/s） | 1,172 ns / 1,222 ns / 19.00（849k/s） | **3.89×** |
| **Suite 2：解析 + 全量 `FIX44.xml` 严格校验 + 读取** | **411 ns** / **421 ns** / **0.00**（2.44M/s） | 2,345 ns / 3,858 ns / 15.00（411k/s） | **5.71×** |
| **Suite 3：TCP Initiator Ping-Pong RTT**（`DirectInitiator`） | **8.78 µs** / **12.15 µs** / **0.00**（111k rt/s） | 21.26 µs / 35.74 µs / 35.06（44.4k rt/s） | **2.42×** |
| **Suite 3：TCP Initiator Ping-Pong RTT**（`Engine busy_poll`） | **10.58 µs** / **15.85 µs** / **0.00**（89.4k rt/s） | 21.26 µs / 35.74 µs / 35.06（44.4k rt/s） | **2.01×** |
| **Suite 3：TCP Initiator Ping-Pong RTT**（`Engine epoll`） | **16.54 µs** / **21.32 µs** / **0.00**（59.1k rt/s） | 21.26 µs / 35.74 µs / 35.06（44.4k rt/s） | **1.29×** |
| **Suite 4：TCP Acceptor Turnaround RTT**（`Engine busy_poll`） | **9.39 µs** / **13.29 µs** / **0.00**（95.9k ord/s） | 20.74 µs / 32.16 µs / 36.58（45.5k ord/s） | **2.21×** |
| **Suite 4：TCP Acceptor Turnaround RTT**（`Engine epoll`） | **11.41 µs** / **17.86 µs** / **0.00**（80.3k ord/s） | 20.74 µs / 32.16 µs / 36.58（45.5k ord/s） | **1.82×** |
| **Suite 5：单批 32 笔同时到达爆发**（`DirectInitiator::send()`，`Burst->Recv`） | **49.89 µs** / **99.76 µs** / **0.00**（338k ord/s） | 178.04 µs / 340.85 µs / 28.06（94.0k ord/s） | **3.57×** |
| **Suite 5：单批 32 笔同时到达爆发**（`enqueue_stream` + `sendmsg`，`Burst->Recv`） | **12.50 µs** / **17.99 µs** / **0.00**（**1.41M ord/s**） | 178.04 µs / 340.85 µs / 28.06（94.0k ord/s） | **14.24×**（吞吐 **15.0×**） |
