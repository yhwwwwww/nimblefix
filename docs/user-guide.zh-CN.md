# NimbleFIX 使用手册

[English](user-guide.md) | **简体中文**

本手册介绍如何使用 NimbleFIX 构建低延迟 FIX 交易客户端、多会话网关与多核服务端。

1. [快速起步与目标选型](#1-快速起步与目标选型)
2. [消息编码与读取](#2-消息编码与读取)
3. [不同场景下的推荐写法](#3-不同场景下的推荐写法)
4. [各模式下的线程模型](#4-各模式下的线程模型)
5. [职责划分：应用程序 vs 库](#5-职责划分应用程序-vs-库)
6. [TLS、主备切换、字典编译、旁路遥测与历史回放](#6-tls主备切换字典编译旁路遥测与历史回放)
7. [多协议版本（FIXT 1.1）、异构网关与运行期运维控制](#7-多协议版本fixt-11异构网关与运行期运维控制)

> 相关文档：[架构设计](architecture.zh-CN.md) · [支持边界矩阵](session-support.zh-CN.md) · [性能基准](benchmarks.zh-CN.md)

---

## 1. 快速起步与目标选型

### 按需选择链接目标

NimbleFIX 将运行时拆分为三层，只需链接你实际用到的部分：

| 业务场景 | 核心 API | CMake / xmake 目标 | 额外依赖 |
| :--- | :--- | :--- | :--- |
| 低延迟单通道交易客户端（1 或 2 线程） | `DirectInitiator`（+ 可选 `DirectSendQueue`） | `NimbleFIX::nimblefix_core` / `nimblefix-core` | C++20 + `pthread` |
| 单通道交易客户端 + TLS / mTLS | `DirectInitiator<..., DirectTlsTransport>` | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | OpenSSL 3+ |
| 多会话客户端/服务端网关、Acceptor 或异步工作池 | `Engine<Schema, Inline\|Queued>` | `NimbleFIX::nimblefix` / `nimblefix` | 可选 `liburing`、`libxml2` |
| 多核高并发 Acceptor 或多通道网关 | `ShardedEngine<Schema, Inline\|Queued>` | `NimbleFIX::nimblefix` / `nimblefix` | 可选 `liburing`、`libxml2` |

### 配置方式

所有组件均通过 C++ 结构体配置（`DirectInitiatorConfig`、`EngineConfig`、`ShardedEngineConfig`、`TlsConfig`、`ConnectionStrategy`、`ReplayOptions`），可按你的项目习惯从 JSON、YAML、TOML、数据库、命令行参数或环境变量填充。

---

## 2. 消息编码与读取

消息编码与网络发送相互正交：同一段流式编码 lambda 可直接复用于 `DirectInitiator`、`DirectSendQueue`、`Engine` 与 `ShardedEngine`。

### 出站：流式编码器（`StreamWriter`）

`StreamWriter` 直接把强类型 FIX 字段写入目标缓冲或 mmap 日志，零堆分配、无中间消息对象：

```cpp
#include <optional>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

const bool is_limit = true;
const std::optional<nf::Decimal> opt_px =
    is_limit ? std::optional{nf::Decimal{15025, 2}} : std::nullopt;

auto write_order = [&](auto& w) {
    // 1. 可选：每笔可动态变化的 Standard Header 字段（必须在正文前写入）。
    //    会话字段（8、9、35、49、56、34、52、43、122、369、10）由引擎自动维护；
    //    像 SenderSubID (50)、TargetSubID (57) 这类动态头字段通过 w.header() 写入：
    // w.header().sender_sub_id("DESK-A").target_sub_id("EXCH-1");

    // 2. 强类型正文字段（原生支持 if/for 与 std::optional<T>）：
    w.cl_ord_id("ORD-0001")
     .symbol("AAPL")
     .side('1')
     .transact_time("20261125-09:30:00.000")
     .order_qty(nf::Decimal{100, 0})
     .ord_type(is_limit ? '2' : '1')
     .price(opt_px); // std::nullopt 时自动跳过

    // 3. 重复组（支持运行期循环与多层嵌套，0 堆分配）：
    w.no_party_i_ds(1, [](auto& g) {
        g.entry()
         .party_id("TRADER-1")
         .party_id_source('D')
         .party_role(1);
    });
};
```

> **其他可选出站方式**：
> - **`Fixed<ByteCap, fix::field::...>`**：适合长期持有、可 `reset()` 原地改写字段并反复重发的固定模板。
> - **`nf::message<"D">()` / `DynamicMessage`**：适合无字典的 `FlatSchema` 模式或运行期动态自定义 Tag。

### 入站：零拷贝强类型视图

```cpp
// DirectInitiator 回调（接收 FlatMessageView）：
nf::Status on_message(const nf::FlatMessageView& msg,
                      const nf::DeliveryInfo& info) noexcept {
    if (msg.is<fix::ExecutionReport>()) {
        const auto er = msg.as<fix::ExecutionReport>();
        const auto exec_id = er.exec_id(); // Result<std::string_view>
        const auto cum_qty = er.cum_qty(); // Result<nf::Decimal>
        // 视图仅借用当次回调的解析/日志缓冲；若需在回调外保留字符串请自行拷贝。
        (void)exec_id; (void)cum_qty; (void)info;
    }
    return nf::Status::success();
}

// Engine / ShardedEngine 按消息类型注册回调：
(void)engine.on<fix::ExecutionReport>(
    [](nf::SessionContext& ctx, const fix::ExecutionReportView& er,
       const nf::DeliveryInfo& info) -> nf::Status {
        const auto cl_ord_id = er.cl_ord_id();
        (void)ctx; (void)cl_ord_id; (void)info;
        return nf::Status::success();
    });
```

---

## 3. 不同场景下的推荐写法

### 场景 A：单通道低延迟客户端 —— 单线程（`DirectInitiator`）

适合同机房策略或报单程序直连单个交易通道，追求最低单笔延迟（单向 ~4.7 µs / RTT ~9 µs）与极简运行时开销。

```cpp
#include <atomic>
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

struct TradingHandler {
    nf::Status on_message(const nf::FlatMessageView& msg,
                          const nf::DeliveryInfo& info) noexcept {
        if (info.is_recovery) {
            // 进程崩溃重启后从本地日志重投的消息：务必做好幂等防护！
        }
        if (msg.is<fix::ExecutionReport>()) {
            const auto er = msg.as<fix::ExecutionReport>();
            (void)er.cl_ord_id();
        }
        return nf::Status::success();
    }
};

int main() {
    (void)nf::pin_current_thread(2); // 可选：把交易/I/O 线程绑定到核心 2

    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = 9876;
    cfg.storage_directory = "./state/buy";
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.timestamp_precision = nf::TimestampPrecision::microseconds; // ms | us | ns

    auto init = nf::DirectInitiator<fix::Schema, TradingHandler>::open(std::move(cfg)).value();
    (void)init.connect();

    while (!init.is_active()) {
        (void)init.poll_once();
        (void)init.reconnect_if_due();
    }

    // 模式 A1 — 单笔最低延迟：直接流式写入 mmap 日志并发出 ::send()
    (void)init.send_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("ORD-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000000")
         .order_qty(nf::Decimal{100, 0}).ord_type('1');
    });

    // 模式 A2 — 同线程攒批（例如一次扫 10 档）：合并为 1 次 sendmsg() 系统调用
    for (int i = 0; i < 10; ++i) {
        (void)init.enqueue_stream<fix::NewOrderSingle>([&](auto& w) {
            w.cl_ord_id("BURST").symbol("AAPL").side('1')
             .transact_time("20261125-09:30:00.000000")
             .order_qty(nf::Decimal{10, 0}).ord_type('1');
        });
    }
    (void)init.flush();

    std::atomic<bool> running{true};
    while (running.load(std::memory_order_relaxed)) {
        (void)init.poll_once();
        (void)init.reconnect_if_due();
        (void)init.poll_maintenance(); // 在热路径外预分配与清理日志分段
    }
    (void)init.logout("shutdown");
}
```

### 场景 B：重计算策略 + 密集爆发报单 —— 双线程无锁流水线（`DirectInitiator` + `DirectSendQueue`）

适合策略计算较重（例如每笔 >1 µs）且经常成批连发的场景，将策略计算与 TCP/日志 I/O 分离到两个 CPU 核上并行推进（峰值可达 1.43M ord/s）。

```cpp
#include <atomic>
#include <thread>
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

// 预分配缓存行对齐的无锁 SPSC 环形队列：512 槽位 × 每槽 512 字节
nf::DirectSendQueue send_queue{512, 512};
std::atomic<bool> stop{false};

// 线程 1（I/O 归属线程 —— 独占操作 init）：
std::thread io_thread([&] {
    (void)nf::pin_current_thread(2);
    auto init = nf::DirectInitiator<fix::Schema, TradingHandler>::open(std::move(cfg)).value();
    (void)init.connect();
    while (!stop.load(std::memory_order_relaxed)) {
        (void)init.drain(send_queue); // 批量弹出最多 64 笔 -> 补序列号/时间戳 -> 写日志 -> 1 次 sendmsg()
        (void)init.poll_once();       // 读回报、驱动心跳与重传
        (void)init.reconnect_if_due();
        (void)init.poll_maintenance();
    }
    (void)init.logout("shutdown");
});

// 线程 2（策略生产者线程 —— send_queue 唯一生产者）：
(void)nf::pin_current_thread(3);
while (!stop.load(std::memory_order_relaxed)) {
    // 0 锁 0 malloc：把正文字段直接流式写入 SPSC 槽位；队列满时返回 ErrorCode::queue_full
    const nf::Status st = send_queue.try_push_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("STRAT-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000")
         .order_qty(nf::Decimal{100, 0}).ord_type('1');
    });
    (void)st;
}
```

### 场景 C：多会话网关或服务端 Acceptor（`Engine<Schema, Inline>` vs `Engine<Schema, Queued>`）

- **`Engine<Schema, nf::Inline>`**：回调快且不阻塞（内存撮合、微秒级风控、立即回 `ExecutionReport`）。延迟最低，业务状态天然单线程。
- **`Engine<Schema, nf::Queued>`**：回调可能阻塞（写库、下游 RPC）。通过有界工作线程池隔离慢回调，确保心跳与 TCP 收包不被阻塞。

```cpp
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

int main() {
    nf::EngineConfig cfg{};
    cfg.storage.directory = "./state/gateway";
    cfg.io_backend = nf::IoBackendConfig::epoll();                // ::epoll() | ::busy_poll() | ::io_uring()
    cfg.io_backend.idle_strategy = nf::IdleStrategy::sleep;       // sleep | busy_spin | hybrid_backoff
    // 使用 Queued 策略时可设置工作线程数：
    // cfg.queued.worker_threads = 4;

    auto engine = nf::Engine<fix::Schema, nf::Inline>::create(cfg).value();

    (void)engine.on<fix::NewOrderSingle>(
        [](nf::SessionContext& ctx, const fix::NewOrderSingleView& req) -> nf::Status {
            return ctx.send_stream<fix::ExecutionReport>([&](auto& w) {
                w.order_id("ORD-1").exec_id("EX-1")
                 .cl_ord_id(req.cl_ord_id().value())
                 .exec_type('0').ord_status('0')
                 .symbol(req.symbol().value()).side(req.side().value())
                 .leaves_qty(req.order_qty().value())
                 .cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
            }).status();
        });

    (void)engine.add_listener({.name = "main", .endpoint = {"0.0.0.0", 9876}});
    auto h = engine.add_acceptor({
        .name = "client-a", .listener = "main",
        .local_comp_id = "EXCH", .remote_comp_id = "CLIENT_A",
    }).value();

    // 跨线程发送句柄（在 engine.run() 前获取）：
    auto mpsc_sender = h.sender<fix::Schema>();        // 多线程并发安全（MPSC）
    auto spsc_lane   = h.producer_lane<fix::Schema>(); // 单生产者无锁通道（SPSC）
    (void)mpsc_sender; (void)spsc_lane;

    // 方式 1：阻塞在当前线程运行（其他线程可调用 engine.request_stop() 停止）
    return engine.run().ok() ? 0 : 1;

    // 方式 2：嵌入你自己的外部事件循环（在同一线程调用 start / poll_once，退出时调用 engine.request_stop()）：
    // (void)engine.start();
    // while (engine.running()) (void)engine.poll_once(/*max_wait_ms=*/1);
}
```

### 场景 D：多核高并发服务端（`ShardedEngine<Schema, Policy>`）

适合在单个监听端口上承载 8–64+ 活跃并发会话。Shard 0 负责接受连接并读取首条 `Logon (35=A)`，按 `(SenderCompID, TargetCompID, BeginString)` 路由后将 socket 直接移交至目标分片的绑定 CPU 核上处理。

```cpp
#include <nimblefix/sharded_engine.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

int main() {
    nf::ShardedEngineConfig cfg{};
    cfg.shard_count = 4;
    cfg.cpu_cores = {2, 3, 4, 5}; // 将 Shard 0..3 绑定到核心 2..5
    cfg.base.storage.directory = "./state/exchange";
    cfg.base.io_backend = nf::IoBackendConfig::epoll();
    cfg.base.io_backend.idle_strategy = nf::IdleStrategy::hybrid_backoff;
    cfg.base.telemetry_path = "/dev/shm/exchange.nfxt"; // 4 个分片共享同一份遥测文件

    auto engine = nf::ShardedEngine<fix::Schema, nf::Inline>::create(cfg).value();

    // 不同分片的回调并发执行；同一会话内的回调严格串行。
    (void)engine.on<fix::NewOrderSingle>(
        [](nf::SessionContext& ctx, const fix::NewOrderSingleView& req) -> nf::Status {
            return ctx.send_stream<fix::ExecutionReport>([&](auto& w) {
                w.order_id("ORD-1").exec_id("EX-1")
                 .cl_ord_id(req.cl_ord_id().value())
                 .exec_type('0').ord_status('0')
                 .symbol(req.symbol().value()).side(req.side().value())
                 .leaves_qty(req.order_qty().value())
                 .cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
            }).status();
        });

    (void)engine.add_listener({.name = "main", .endpoint = {"0.0.0.0", 9876}});
    for (int i = 0; i < 16; ++i) {
        (void)engine.add_acceptor({
            .name = "client-" + std::to_string(i),
            .listener = "main",
            .local_comp_id = "EXCH",
            .remote_comp_id = "BUY_" + std::to_string(i),
        });
    }

    // 启动 K-1 个分片线程，并在当前线程运行 Shard 0
    return engine.run().ok() ? 0 : 1;
}
```

### 场景选型速查表

| 业务特征 | 推荐组合 | 原因 |
| :--- | :--- | :--- |
| 单通道、低频/单笔极低延迟 | `DirectInitiator::send_stream()`（单线程） | 单向延迟最低（**4.62 µs**），零线程切换 |
| 单通道、单线程连发多笔订单 | `DirectInitiator::enqueue_stream()` + `flush()` | 合并为 1 次 `sendmsg()`（**1.44M ord/s**） |
| 单通道、重策略计算 + 密集连发 | `DirectInitiator` + `DirectSendQueue`（双线程） | 策略计算与 I/O 并行（2 µs/笔计算下 **427k ord/s**、`Burst->Recv` 延迟减半；开放循环 p99 **24.1 µs**） |
| 2–8 个会话、回调快且无阻塞 | `Engine<Schema, Inline>`（`epoll` / `io_uring`） | 单协议线程无锁，应用状态天然单线程 |
| 多会话、回调含数据库/RPC 阻塞 | `Engine<Schema, Queued>` | 慢回调与协议心跳/收包解耦 |
| 8–64+ 高并发服务端 | `ShardedEngine<Schema, Inline>`（`io_uring` + `hybrid_backoff` 绑核） | 单端口多核扩展（**447k ord/s**，p99 降低 4.11×） |

---

## 4. 各模式下的线程模型

NimbleFIX 所有运行时都遵循**单写者协议归属（Single-Writer Protocol Ownership）**：任意一个 FIX 会话始终只由唯一一个线程（**协议归属线程**）操作其 `SessionCore`、序列号、socket 与 mmap 日志，协议热路径完全无锁。

### 4.1 `DirectInitiator`（单线程，无后台线程）

```text
[ 调用方 / 交易线程（协议归属线程） ]
   ├── send_stream() / enqueue_stream()+flush() ──▶ mmap 日志 ──▶ TcpTransport / TLS
   ├── poll_once() ──▶ 读 socket ──▶ 解析校验 ──▶ mmap 日志 ──▶ Handler::on_message()
   ├── reconnect_if_due() ──▶ FailoverController
   └── poll_maintenance() ──▶ 预分配/清理日志分段
```

- **库创建的线程数**：`0`。
- **回调执行线程**：`Handler::on_message()` 在 `poll_once()` 内同步执行。
- **规则**：不要从多个线程并发访问同一个 `DirectInitiator` 实例。

### 4.2 `DirectInitiator` + `DirectSendQueue`（双线程无锁 SPSC）

```text
[ 策略线程（唯一生产者） ]                           [ I/O 线程（协议归属线程 / 唯一消费者） ]
   └── queue.try_push_stream<M>(fn)                    ├── init.drain(queue) ──▶ mmap 日志 ──▶ sendmsg()
             │                                         ├── init.poll_once()  ──▶ Handler::on_message()
             ▼  （无锁 SPSC 环，acquire/release）       └── init.poll_maintenance()
     [ DirectSendQueue ] ────────────────────────────▶
```

- **库创建的线程数**：`0`（两个线程均由应用程序创建并绑核）。
- **回调执行线程**：`Handler::on_message()` 运行在 **I/O 线程**。如策略线程需要回报，请通过你自己的 SPSC 队列或原子结构回传。
- **规则**：同一个 `DirectSendQueue` 严格只允许 1 个生产者线程写入、1 个消费者线程调用 `init.drain(queue)`。

### 4.3 `Engine<Schema, Inline>`（单协议线程 + 可选外部发单线程）

```text
[ 外部业务线程（任意数量） ]
   ├── Sender::send_stream()        ──▶ (有界 MPSC 队列) ──────────────┐
   ├── ProducerLane::send_stream()  ──▶ (无锁 SPSC，限 1 线程) ────────┼──▶ eventfd 唤醒
   └── engine.request_stop() / snapshot()                             │
                                                                      ▼
                                             [ 协议归属线程：engine.run() ]
                                                ├── IoBackend (epoll / busy_poll / io_uring)
                                                ├── 全部 SessionCore 与 mmap 日志（0 锁）
                                                └── 全部业务回调在此线程内联执行
```

- **库创建的线程数**：`0`（`engine.run()` 运行在调用线程）。
- **回调执行线程**：`on<M>()`、`on_message()`、`on_session_active()`、`on_error()`、`AuthenticationConfig::verify` 均在 `engine.run()` 线程按序执行。
- **规则**：`Inline` 回调必须快且不阻塞；阻塞会拖慢同一 `Engine` 内全部会话的心跳与收包。

### 4.4 `Engine<Schema, Queued>`（单协议线程 + N 个工作线程）

```text
[ 协议归属线程：engine.run() ]                   [ 工作线程池：cfg.queued.worker_threads ]
   ├── Socket I/O、解析校验、心跳                  ├── Worker 1 ──┐
   ├── 入站消息落盘到 mmap 日志                    ├── Worker 2 ──┼─▶ on<M>(ctx, view)
   └── 投入每会话串行队列 ───────────────────────▶└── Worker N ──┘     └── ctx.send_stream()（回投协议线程）
         （同一会话同时最多 1 个回调在途）
```

- **库创建的线程数**：`cfg.queued.worker_threads` 个工作线程（默认 `2`）。
- **回调执行规则**：
  - **同一会话内**：按 `MsgSeqNum` 严格串行（同时最多 1 个回调运行），前后回调即使落到不同工作线程也保证 happens-before 可见性。
  - **不同会话间**：在工作线程上**并行执行**，共享应用状态需自行同步。
  - `AuthenticationConfig::verify`（`SessionSettings::authentication`）仍在协议归属线程、打开会话日志之前执行。

### 4.5 `ShardedEngine<Schema, Policy>`（K 个绑核分片 Reactor）

```text
                                 [ 共享监听端口（如 :9876） ]
                                              │
                                    (accept + Logon 35=A)
                                              ▼
       ┌───────────────────────────────[ Shard 0（调用线程，核心 c0） ]
       │      （通过无锁 fd 队列移交连接）       ├── 归属 Shard 0 的会话
       ├───────────────────────────┐           └── （若为 Queued 则含独立工作池 0）
       ▼                           ▼
[ Shard 1（线程 1，核心 c1） ]   [ Shard K-1（线程 K-1，核心 cK-1） ]
  ├── 归属 Shard 1 的会话          ├── 归属 Shard K-1 的会话
  └── （可选工作池 1）             └── （可选工作池 K-1）
```

- **库创建的线程数**：`K - 1` 个分片线程（Shard 0 在调用 `engine.run()` 的线程运行）；若 `Policy == Queued`，每个分片再各自创建 `worker_threads` 个工作线程。
- **回调执行规则**：每个会话静态绑定到唯一分片（单会话内严格串行）；**不同分片上的会话在不同 CPU 核上并行运行**，跨分片共享的业务状态必须同步或按分片分区。

### 4.6 对象与句柄线程安全性速查

| 对象 / 句柄 | 线程安全性 | 使用规则 |
| :--- | :--- | :--- |
| `DirectInitiator` | 非线程安全 | 仅由单个 I/O 线程调用 |
| `DirectSendQueue` | 无锁 SPSC（最多 2 线程） | 1 个生产者（`try_push*`）+ 1 个消费者（`init.drain`） |
| `Engine` / `ShardedEngine` 配置期方法（`on`、`add_*`） | 仅启动前调用 | 在 `run()` / `start()` 前完成配置 |
| `Engine::request_stop()`、`snapshot()` 及运行期控制接口 | 线程安全 | 运行期可由任意线程调用 |
| `Sender<Schema>`（`handle.sender()`） | 线程安全（MPSC） | 可在任意多个生产者线程间共享；通过 `Sender::receipt(id)` 查询回执 |
| `ProducerLane<Schema>`（`handle.producer_lane()`） | 无锁 SPSC（单生产者） | 绑定首次调用线程（跨线程返回 `wrong_thread`）；通过 `ProducerLane::receipt(id)` 查询回执 |
| `SessionContext`、`FlatMessageView`、`{Msg}View` | 仅当次回调有效 | 不可保存到回调之外；需要留存时请拷贝字段数据 |
| `TelemetryReader` | 只读、跨进程安全 | `O_RDONLY`/`PROT_READ` 映射 `.nfxt`，零锁无干扰 |
| `JournalTailer` | 只读、跨进程无锁 SPMC | `O_RDONLY`/`PROT_READ` 映射 `.nfxj` 分段（不抢 `LOCK_EX`），绝不阻塞交易写端；单实例单线程调用 |
| `JournalReplayer` | 在线/离线均安全 | 按路径回放使用跨进程无锁 `JournalTailer`；进程内 `Journal&` 重载使用带租约 `JournalReader` |

---

## 5. 职责划分：应用程序 vs 库

| 领域 | **NimbleFIX 自动管理** | **应用程序自行管理** |
| :--- | :--- | :--- |
| **配置与凭据** | 校验全部 C++ 配置结构体；持久化日志时自动脱敏 Logon 密码。 | 从文件/数据库/环境变量读取配置、轮换密钥并填充配置结构体。 |
| **事件循环与线程** | `Engine::run()` / `ShardedEngine::run()` 驱动 `IoBackend`、定时器、分片线程、绑核（`cpu_core`/`cpu_cores`）及 `Queued` 线程池。 | 使用 `DirectInitiator`（或手动 `Engine::start()` + `poll_once()`）时，持续调用 `poll_once()`、`drain()`、`reconnect_if_due()`、`poll_maintenance()`；自建/绑定业务线程；捕获 `SIGINT`/`SIGTERM` 并调用 `request_stop()`。 |
| **回调安全与生命周期** | 单会话按 `MsgSeqNum` 严格有序投递；回调期间零拷贝视图有效。 | 回调外需要使用的 `std::string_view` 须自行拷贝；在 `Queued` / `ShardedEngine` 下同步跨会话共享状态；保证 `Inline` 回调不阻塞。 |
| **崩溃恢复与幂等性** | 入站先落盘再投递、出站先落盘再发送；崩溃/`SIGKILL` 重启后重建序列号，重投未完成入站消息（`DeliveryInfo::is_recovery == true`）并复用已落盘的关联回复。 | 当 `info.is_recovery == true` 时保证外部副作用（下单路由、写库、风控扣减）**幂等**（跨崩溃为至少一次投递）。 |
| **FIX 会话协议** | FIX 4.2/4.3/4.4/FIXT.1.1 的 Logon、Heartbeat、TestRequest、`MsgSeqNum`、`ResendRequest`、`GapFill`、业务 `Replay`（`43=Y`、`122=OrigSendingTime`）、`369`（`LastMsgSeqNumProcessed`）、Session Reject (`35=3`)、优雅 Logout。 | 业务订单状态机、`BusinessMessageReject (35=j)` 规则，以及 Acceptor 场景在 `SessionSettings::authentication`（固定凭据或 `AuthenticationConfig::verify` 回调）中校验对端凭据。 |
| **TCP / TLS 与主备切换** | 非阻塞 TCP（`TCP_NODELAY`、`sendmsg` 批发送）、OpenSSL 3 TLS 1.2/1.3 + CA/SAN/mTLS + 重连复用 `TlsContext`、多节点主备切换（`ordered`/`round_robin`、指数退避、重试上限）。 | 提供数字 IPv4/IPv6 地址（如需域名解析由应用完成）、维护证书与私钥文件、配置了有限 `max_retries` 时处理 `FailoverController::exhausted()`。 |
| **系统时钟与 `SendingTime`** | 格式化 UTC `52=SendingTime`（`ms`/`us`/`ns`）并校验对端时钟偏差（`max_sending_time_skew_ms`，默认 120 s）。 | 通过 NTP / PTP 保持宿主机时钟同步。 |
| **存储与持久性边界** | 预分配分段 `MAP_SHARED` mmap 日志、SSE4.2 CRC32C、原子建段、段轮转、读租约保护、满容 fail-closed。 | 规划磁盘空间（`capacity_bytes`、`segment_bytes`）、监控磁盘健康，并知晓 `appended` 保障**同机进程崩溃恢复**（热路径不做逐消息 `fsync`）。 |
| **可观测性与回放** | 设置 `telemetry_path` 后无锁输出会话计数与 5 阶段 HDR 延迟直方图（`read`/`parse`/`validate`/`journal`/`write`）；提供 `TelemetryReader` 与 `JournalReplayer`。 | 部署旁路采集/Prometheus Exporter 及历史日志归档。 |

---

## 6. TLS、主备切换、字典编译、旁路遥测与历史回放

### 6.1 TLS / mTLS

- **`DirectInitiator`**：链接 `NimbleFIX::nimblefix_core_tls` 并传入 `nf::DirectTlsTransport`：
  ```cpp
  nf::TlsConfig tls{};
  tls.enabled = true;
  tls.ca_file = "/etc/certs/ca.pem";
  tls.server_name = "fix.venue.example";
  tls.minimum_version = 13; // TLS 1.3（或 12 表示 TLS 1.2+）
  // 双向 mTLS 补充以下两行：
  // tls.certificate_file = "/etc/certs/client.pem";
  // tls.private_key_file = "/etc/certs/client.key";

  auto transport = nf::DirectTlsTransport::create(std::move(tls)).value();
  transport.set_socket_options({.tcp_quickack = true}); // 或直接配置 DirectInitiatorConfig::socket_options
  auto init = nf::DirectInitiator<fix::Schema, TradingHandler, nf::DirectTlsTransport>::open(
      std::move(cfg), TradingHandler{}, std::move(transport)).value();
  ```
- **`Engine` / `ShardedEngine`**：直接设置 `ListenerConfig::tls`（服务端）或 `InitiatorConfig::tls`（客户端）。

### 6.2 多节点主备切换（`ConnectionStrategy`）

`DirectInitiatorConfig` 与 `InitiatorConfig` 配置方式完全一致：

```cpp
cfg.endpoints = {
    {"10.0.1.10", 9876}, // 主节点
    {"10.0.2.10", 9876}, // 备节点
};
// 主节点优先：失败立即（0 ms）切备；一轮全失败后 250ms -> 500ms -> ... -> 10s 指数退避；
// 成功建链并断线后自动回归主节点：
cfg.connection_strategy = nf::ConnectionStrategy::ordered(
    /*initial_ms=*/250, /*max_ms=*/10'000, /*multiplier=*/2,
    /*retries=*/0, /*reset_primary=*/true, /*immediate=*/true);
// 或轮询模式：nf::ConnectionStrategy::round_robin(250, 10'000, 2);
```

### 6.3 字典代码生成（`nimblefix-schema-gen`）

将标准或交易所自定义 FIX XML 字典编译为强类型 C++20 头文件：

```sh
# 1. 生成完整字典：
nimblefix-schema-gen --output generated/fix44.hpp --namespace my::fix44 schemas/standard/FIX44.xml

# 2. 按业务消息子集裁剪（自动保留管理消息与关联重复组，加速编译）：
nimblefix-schema-gen --output generated/venue.hpp --namespace my::venue \
  --messages NewOrderSingle,OrderCancelRequest,ExecutionReport schemas/standard/FIX44.xml

# 3. 从真实 SOH / 竖线('|') / .nfxj 日志自动裁剪未用到的消息与可选字段：
nimblefix-schema-gen --output generated/venue.hpp --namespace my::venue \
  --from-fix-log ./captures/session.log schemas/standard/FIX44.xml

# 4. 联合编译 FIXT 1.1 会话层 + FIX 5.0 SP2 应用层字典：
nimblefix-schema-gen --output generated/fix50sp2.hpp --namespace my::fix50sp2 \
  schemas/standard/FIXT11.xml schemas/standard/FIX50SP2.xml
```

### 6.4 旁路 mmap 遥测（`TelemetryReader`）

在 `DirectInitiatorConfig` 或 `EngineConfig` 中设置 `telemetry_path = "/dev/shm/app.nfxt"` 与 `telemetry_stage_probes = true`，即可在独立监控进程中零干扰读取实时指标与各阶段延迟分位数：

```cpp
#include <iostream>
#include <nimblefix/telemetry.hpp>

auto reader = nimblefix::TelemetryReader::open("/dev/shm/app.nfxt").value();
for (const auto& s : reader.read_all()) {
    const auto& parse_lat = s.stages[static_cast<std::size_t>(nimblefix::TelemetryStage::parse)];
    std::cout << s.name << " rx=" << s.inbound_frames
              << " tx=" << s.outbound_frames
              << " unexpected=" << s.unexpected_fields
              << " parse_p99_ns=" << parse_lat.p99_ns << '\n';
}
// 输出 Prometheus 文本格式：
// std::cout << reader.format_prometheus();
```

### 6.5 跨进程实时追尾（`JournalTailer`）与历史回放（`JournalReplayer`）

`JournalTailer` 为在线或离线 `.nfxj` 会话日志提供类似 Chronicle Queue 的跨进程无锁 SPMC 只读追尾（适合 DropCopy、实时风控与合规旁路进程）。多个独立进程可并发追尾同一个活跃交易会话，对交易写端热路径零额外开销：

```cpp
#include <nimblefix/journal.hpp>

nimblefix::JournalTailerConfig tcfg{};
tcfg.path = "./state/buy/sessions/<hex-key>/session.nfxj"; // string_view：open() 期间需保持底层字符串存活
tcfg.start = nimblefix::JournalTailStart::beginning; // 或 ::end / ::from_id
tcfg.verify_crc = true;

auto tailer = nimblefix::JournalTailer::open(tcfg).value();
nimblefix::JournalRecord rec{};
for (;;) {
    const auto st = tailer.next(rec); // 非阻塞：追到队尾返回 ErrorCode::need_more
    if (st.code == nimblefix::ErrorCode::need_more) continue;
    if (!st.ok()) break;              // 如滞后超出已回收分段返回 ErrorCode::reader_expired
    // rec.payload 为零拷贝视图，下次调用 next() 前有效
    (void)rec.seq; (void)rec.kind; (void)rec.payload;
}
```

如需按条件确定性回放（在线或离线），可使用 `JournalReplayer`（含自定义 `DATA`/`LENGTH` 配对时可设置 `opts.wire_hints`）：

```cpp
#include <nimblefix/replay.hpp>

nimblefix::ReplayOptions opts{};
opts.journal_path = "./state/buy/sessions/<hex-key>/session.nfxj";
opts.direction = nimblefix::ReplayDirection::inbound;
opts.timing = nimblefix::ReplayTiming::scaled;
opts.speed_multiplier = 10.0; // 按原始 52=SendingTime 间隔 10 倍速回放
opts.skip_admin = true;

// 方式 1：进程内回调接收（回测 / 复现）
auto stats = nimblefix::JournalReplayer::replay(opts,
    [](const nimblefix::JournalRecord& rec, const nimblefix::FlatMessageView* parsed) noexcept {
        (void)rec; (void)parsed;
        return nimblefix::Status::success();
    }).value();

// 方式 2：原样通过 TCP 灌入测试环境 Acceptor
// auto stats = nimblefix::JournalReplayer::replay_to_tcp(opts, {"127.0.0.1", 9876}).value();
```

### 6.6 可选 `nimblefix` 命令行工具

编译开启 `-DNIMBLEFIX_BUILD_CLI=ON`（或 `--cli=y`）后，可使用独立可执行程序 `nimblefix` 进行快速运维操作：

```sh
nimblefix doctor --json
nimblefix init --directory ./demo
nimblefix validate --config ./demo/acceptor.toml --json
nimblefix run --config ./demo/acceptor.toml
nimblefix telemetry --path ./demo/telemetry.nfxt --prometheus
nimblefix replay --path ./demo/state/.../session.nfxj --schema fix44 --direction inbound --timing original --endpoint 127.0.0.1:9876
nimblefix schema generate --xml schemas/standard/FIXT11.xml --xml schemas/standard/FIX50SP2.xml --namespace venue --output venue_50sp2.hpp
```

---

## 7. 多协议版本（`FIXT 1.1`）、异构网关与运行期运维控制

### 7.1 `FIXT.1.1` + `FIX 5.0 SP2` 强类型会话

`FIX 5.0 SP2` 将 `FIXT.1.1` 会话传输层（`FIXT11.xml`）与应用层字典（`FIX50SP2.xml`）解耦。将两个 XML 同时传给 `nimblefix-schema-gen`（顺序不限）或直接链接内置 `fix50sp2` 目标，配置 `FixVersion::fixt11` 与 `default_appl_ver_id = nf::appl_ver_id::fix50sp2`，并可按需在单笔消息头写入 `1128=ApplVerID`：

```cpp
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix50sp2.hpp>
#include <nimblefix/protocol.hpp>

namespace nf    = nimblefix;
namespace fix50 = nimblefix::fix50sp2;

nf::DirectInitiatorConfig cfg{};
cfg.version = nf::FixVersion::fixt11;
cfg.default_appl_ver_id = std::string{nf::appl_ver_id::fix50sp2}; // Logon 协商 1137=9
cfg.host = "127.0.0.1";
cfg.port = 9876;
cfg.storage_directory = "./state/fix50";
cfg.local_comp_id = "TRADER50";
cfg.remote_comp_id = "VENUE50";

// 单笔可选携带 1128=ApplVerID（与 DefaultApplVerID 一致时亦可省略）：
auto write_50sp2_order = [](auto& w) {
    w.header().appl_ver_id(nf::appl_ver_id::fix50sp2);
    w.cl_ord_id("ORD50-1").symbol("NVDA").side('1')
     .transact_time("20261127-09:30:00.000")
     .order_qty(nf::Decimal{100, 0}).ord_type('1');
};
```

### 7.2 同引擎异构多字典（`Engine<FlatSchema>`）与未知扩展 Tag 策略

单个 `Engine<nf::FlatSchema, Policy>` 或 `ShardedEngine<nf::FlatSchema, Policy>` 可同时承载不同 FIX 版本与自定义柜台字典的会话，并按会话配置未登记扩展 Tag 的处理策略（`UnexpectedFieldPolicy`）：

```cpp
nf::AcceptorConfig leg42{.name = "client-42", .listener = "main", .local_comp_id = "GW", .remote_comp_id = "BUY42"};
leg42.settings.version = nf::FixVersion::fix42;
leg42.settings.use_schema<nf::fix42::Schema>();
// reject（默认拒绝） | ignore_with_metric（放行并计入 TelemetrySessionSnapshot::unexpected_fields） | passthrough（静默放行）
leg42.settings.unexpected_field_policy = nf::UnexpectedFieldPolicy::ignore_with_metric;

nf::AcceptorConfig leg50{.name = "client-50", .listener = "main", .local_comp_id = "GW", .remote_comp_id = "BUY50"};
leg50.settings.version = nf::FixVersion::fixt11;
leg50.settings.default_appl_ver_id = std::string{nf::appl_ver_id::fix50sp2};
leg50.settings.use_schema<nf::fix50sp2::Schema>();
```

### 7.3 运行期运维控制、日切自动重置与 `SocketOptions`

- **在线/离线会话运维接口**（`Engine`、`ShardedEngine`、`SessionDriver`、`DirectInitiator` 均支持）：
  ```cpp
  (void)engine.request_logout("client-42", "maintenance"); // 发起优雅 35=5 Logout 握手
  (void)engine.request_sequence_reset("client-42");        // 武装下次 Logon 一次性 141=Y（若在线则先优雅登出）
  (void)engine.request_reconnect("venue-a");               // 立即断线重连
  // 在会话断开状态下持久化调整下一期望入站/出站序列号（均须 >= 1）：
  (void)engine.adjust_sequences("client-42", /*next_in=*/100, /*next_out=*/250);
  ```
- **交易时段边界自动重置序号（EOD/BOD）**：同时开启 `settings.allow_sequence_reset = true`、`settings.schedule.enabled = true` 与 `settings.schedule.reset_on_schedule_boundary = true`，即可在配置的 UTC 会话时间窗切换时自动推进 Epoch 并重置序列号。
- **低延迟套接字调优（`SocketOptions`）**：在 `DirectInitiatorConfig`、`InitiatorConfig`、`ListenerConfig` 或 `DirectTlsTransport::set_socket_options(sock_opts)` 上配置数字源地址绑定（`local_bind_ip` / `local_bind_port`）、`tcp_quickack = true`（Linux 读后自动重武装）、`so_busy_poll_us` 以及 `send_buffer_bytes` / `recv_buffer_bytes`。
