# NimbleFIX 0.3 使用手册

[English](user-guide.md) | **简体中文**

本手册是使用 NimbleFIX 0.3 构建生产级 FIX 交易客户端、网关与多核服务端的实战指南，重点涵盖：

1. [库设计哲学与分层选型](#1-库设计哲学与分层选型)
2. [消息构造与读取（零堆分配流式 API）](#2-消息构造与读取零堆分配流式-api)
3. [不同业务场景下的推荐写法](#3-不同业务场景下的推荐写法)
4. [各使用模式下的详细线程模型](#4-各使用模式下的详细线程模型)
5. [职责边界矩阵：用户自行管理 vs 库自动管理](#5-职责边界矩阵用户自行管理-vs-库自动管理)
6. [运维与进阶能力：TLS、主备切换、字典编译、旁路遥测与历史回放](#6-运维与进阶能力tls主备切换字典编译旁路遥测与历史回放)

> 相关文档：[架构设计](architecture.md) · [支持边界矩阵](session-support.md) · [性能基准](benchmarks.md) · [路线图](roadmap.md)

---

## 1. 库设计哲学与分层选型

### 纯 C++ 配置接口（零配置文件格式偏好）

NimbleFIX 是一个**基础库**，而不是绑死配置格式的框架。库目标（`NimbleFIX::nimblefix_core`、`NimbleFIX::nimblefix_core_tls`、`NimbleFIX::nimblefix`）**不直接读取任何配置文件**，也**不对 TOML / JSON / YAML / INI / 数据库做任何偏好假设**。

所有组件都只接收强类型、自带合法性校验的纯 C++20 结构体：
- `DirectInitiatorConfig`（`<nimblefix/direct_initiator.hpp>`）
- `EngineConfig`、`ListenerConfig`、`AcceptorSessionConfig`、`InitiatorSessionConfig`（`<nimblefix/engine.hpp>`）
- `ShardedEngineConfig`（`<nimblefix/sharded_engine.hpp>`）
- `TlsConfig`（`<nimblefix/tls_config.hpp>`）
- `ConnectionStrategy`（`<nimblefix/connection_strategy.hpp>`）
- `ReplayOptions`（`<nimblefix/replay.hpp>`）

配置文件采用什么格式、如何解析、如何注入密钥与热更新，完全由你的应用程序决定。`tools/nimblefix` 中的独立 CLI 工具使用 TOML 仅作为应用层示例，不被任何库目标依赖。

### 按需选择三层之一

| 你的业务场景 | 推荐层级与核心类 | 链接目标 (CMake / xmake) | 额外依赖 |
| :--- | :--- | :--- | :--- |
| 极低延迟单通道交易客户端（1 或 2 线程） | **Layer 0** `DirectInitiator`（+ 可选 `DirectSendQueue`） | `NimbleFIX::nimblefix_core` / `nimblefix-core` | 仅 C++20 + `pthread` |
| 单通道交易客户端 + TLS / mTLS | **Layer 0 + TLS** `DirectInitiator<..., DirectTlsTransport>` | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | OpenSSL 3+ |
| 多会话客户端/服务端网关、Acceptor 或异步回调隔离 | **Layer 1/2** `Engine<Schema, Inline\|Queued>` | `NimbleFIX::nimblefix` / `nimblefix` | 可选 `liburing`、`libxml2` |
| 多核高并发 Acceptor / 多通道网关 | **Layer 1/2** `ShardedEngine<Schema, Inline\|Queued>` | `NimbleFIX::nimblefix` / `nimblefix` | 可选 `liburing`、`libxml2` |

---

## 2. 消息构造与读取（零堆分配流式 API）

**怎么构造消息**与**怎么通过网络发送**是完全正交的两件事，可以任意组合。

### 出站编码：`StreamWriter`（默认主力，0 堆分配，~130 ns）

`StreamWriter` 不构造中间消息对象、不排序槽位、不额外拷贝字符串，直接在 `lambda` 中按强类型字段名流式写入目标缓冲（在 `DirectInitiator` 中直接写入 mmap Journal 槽位）：

```cpp
#include <optional>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

const bool is_limit = true;
const std::optional<nf::Decimal> opt_px =
    is_limit ? std::optional{nf::Decimal{15025, 2}} : std::nullopt;

// 同一段 lambda 可直接传给 DirectInitiator::send_stream / enqueue_stream、
// DirectSendQueue::try_push_stream、SessionContext::send_stream、
// Sender::send_stream、ProducerLane::send_stream 以及 encode_fix44_stream：
auto write_order = [&](auto& w) {
    // 1. 可选：每笔消息可动态变化的 Standard Header 字段（必须在正文前写入）。
    //    引擎自动维护 8/9/35/49/56/34/52/43/122/369/10；
    //    像 SenderSubID (50)、TargetSubID (57) 这类同会话内会变的头字段通过 w.header() 写：
    // w.header().sender_sub_id("DESK-A").target_sub_id("EXCH-1");

    // 2. 强类型正文字段 —— 原生支持 C++ if/for 与 std::optional<T>：
    w.cl_ord_id("ORD-0001")
     .symbol("AAPL")
     .side('1')
     .transact_time("20261125-09:30:00.000")
     .order_qty(nf::Decimal{100, 0})
     .ord_type(is_limit ? '2' : '1')
     .price(opt_px); // std::nullopt 时自动跳过

    // 3. 重复组（支持运行期动态循环与多层嵌套，0 堆分配）：
    w.no_party_i_ds(1, [](auto& g) {
        g.entry()
         .party_id("TRADER-1")
         .party_id_source('D')
         .party_role(1);
    });
};
```

> **其他可选出站方式**：
> - **`Fixed<ByteCap, fix::field::...>`**：适合需要长期持有、可 `reset()` 原地改字段并反复重发的固定模板消息。
> - **`nf::message<"D">()` / `DynamicMessage`**：适合无字典 Flat 模式（`FlatSchema`）或编译期未知的动态自定义 Tag。

### 入站读取：零拷贝强类型视图（O(1) 索引直取）

```cpp
// DirectInitiator 回调（接收 FlatMessageView）：
nf::Status on_message(const nf::FlatMessageView& msg,
                      const nf::DeliveryInfo& info) noexcept {
    if (msg.is<fix::ExecutionReport>()) {
        const auto er = msg.as<fix::ExecutionReport>();
        const std::string_view exec_id = er.exec_id().value_or("");
        const auto cum_qty = er.cum_qty(); // std::optional<nf::Decimal>
        // 注意：视图仅借用当次回调的解析缓冲，离开回调即失效；若需留存请自行拷贝！
        (void)exec_id; (void)cum_qty; (void)info;
    }
    return nf::Status::success();
}

// Engine / ShardedEngine 按消息类型强类型注册回调：
(void)engine.on<fix::ExecutionReport>(
    [](nf::SessionContext& ctx, const fix::ExecutionReportView& er,
       const nf::DeliveryInfo& info) -> nf::Status {
        const auto cl_ord_id = er.cl_ord_id();
        (void)ctx; (void)cl_ord_id; (void)info;
        return nf::Status::success();
    });
```

---

## 3. 不同业务场景下的推荐写法

### 场景 A：极低延迟单通道客户端 —— 单线程（`DirectInitiator`）

**适用场景**：同机房策略/执行程序直连单个交易所或券商，追求最低单笔延迟（单向 ~4.7 µs / RTT ~9 µs）与极简二进制体积（~336 KB）。

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
            // 崩溃重启后的本地恢复重投：务必先做业务幂等判断！
        }
        if (msg.is<fix::ExecutionReport>()) {
            const auto er = msg.as<fix::ExecutionReport>();
            (void)er.cl_ord_id();
        }
        return nf::Status::success();
    }
};

int main() {
    (void)nf::pin_current_thread(2); // 可选：把交易/I/O 线程绑定到隔离核心 2

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

    // 模式 A1 — 最低单笔延迟：直接流式编码进 mmap Journal 并立即 ::send()
    auto seq = init.send_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("ORD-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000000")
         .order_qty(nf::Decimal{100, 0}).ord_type('1');
    });
    (void)seq;

    // 模式 A2 — 同线程密集攒批（例如一次扫 10 档）：合并为 1 次 sendmsg() 系统调用
    for (int i = 0; i < 10; ++i) {
        (void)init.enqueue_stream<fix::NewOrderSingle>([&](auto& w) {
            w.cl_ord_id("BURST").symbol("AAPL").side('1')
             .transact_time("20261125-09:30:00.000000")
             .order_qty(nf::Decimal{10, 0}).ord_type('1');
        });
    }
    (void)init.flush();

    // 主循环：由调用方持续推进收发、心跳、重连及热路径外的 Journal 段预分配维护
    std::atomic<bool> running{true};
    while (running.load(std::memory_order_relaxed)) {
        (void)init.poll_once();
        (void)init.reconnect_if_due();
        (void)init.poll_maintenance();
    }
    (void)init.logout("shutdown");
}
```

### 场景 B：重计算策略 + 密集爆发报单 —— 双线程无锁流水线（`DirectInitiator` + `DirectSendQueue`）

**适用场景**：策略线程本身有较重计算（如每笔 >1 µs）且经常成批连发，希望策略核与网络/落盘核并行流水线、消除队头阻塞（峰值 **1.43M ord/s**，有策略计算时 CO-Free 延迟直接腰斩）。

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
        (void)init.drain(send_queue); // 批量弹出最多 64 笔 -> 补序列号/时间戳 -> 落盘 -> 1 次 sendmsg()
        (void)init.poll_once();       // 读回报、驱动心跳与重传
        (void)init.reconnect_if_due();
        (void)init.poll_maintenance();
    }
    (void)init.logout("shutdown");
});

// 线程 2（策略生产者线程 —— send_queue 的唯一生产者）：
(void)nf::pin_current_thread(3);
while (!stop.load(std::memory_order_relaxed)) {
    // 0 锁 0 malloc：把正文字段直接流式写入 SPSC 槽位；队列满时返回 ErrorCode::queue_full（背压）
    const nf::Status st = send_queue.try_push_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("STRAT-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000")
         .order_qty(nf::Decimal{100, 0}).ord_type('1');
    });
    (void)st;
}
```

### 场景 C：多会话网关或服务端 Acceptor（`Engine<Schema, Inline>` vs `Engine<Schema, Queued>`）

**选型原则**：
- **`Engine<Schema, nf::Inline>`**：回调快且不阻塞（撮合/内存风控/立即回 `ExecutionReport`），延迟最低、零线程切换。
- **`Engine<Schema, nf::Queued>`**：回调可能阻塞（写数据库、下游 RPC、重型风控），通过有界工作线程池隔离慢回调，确保心跳和 TCP 接收绝不饿死。

```cpp
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

int main() {
    nf::EngineConfig cfg{};
    cfg.storage.directory = "./state/gateway";
    cfg.io_backend = nf::IoBackendConfig::epoll(); // ::epoll() | ::busy_poll() | ::io_uring()
    cfg.idle_strategy = nf::IdleStrategy::sleep;   // sleep | busy_spin | hybrid_backoff
    // 使用 Queued 策略时可配置工作线程数：
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

    // 跨线程发单句柄（在 engine.run() 之前获取）：
    auto mpsc_sender = h.sender<fix::Schema>();        // 任意多线程并发安全（MPSC）
    auto spsc_lane   = h.producer_lane<fix::Schema>(); // 单生产者极致低延迟无锁队列（SPSC）
    (void)mpsc_sender; (void)spsc_lane;

    // 方式 1：阻塞在当前线程运行（其他线程可调用 engine.request_stop() 优雅退出）
    return engine.run().ok() ? 0 : 1;

    // 方式 2：嵌入你自己的外部事件循环（同一线程调用 start / poll_once / stop）：
    // (void)engine.start();
    // while (running) (void)engine.poll_once(/*max_wait_ms=*/1);
    // (void)engine.stop();
}
```

### 场景 D：多核高并发服务端（`ShardedEngine<Schema, Policy>`）

**适用场景**：需要同时承载 8–64+ 活跃并发会话（券商柜台网关、交易所报盘接入、多通道路由），单核 Reactor 已成瓶颈。所有 Acceptor 共享**同一个监听端口**：Shard 0 负责 `accept` 与读取首条 `Logon (35=A)`，按 `(SenderCompID, TargetCompID, BeginString)` 路由后将 socket 直接移交至目标分片绑定核心的 Reactor，稳态收发 **100% 在所属分片核上无锁运行**。

```cpp
#include <nimblefix/sharded_engine.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

int main() {
    nf::ShardedEngineConfig cfg{};
    cfg.shard_count = 4;
    cfg.cpu_cores = {2, 3, 4, 5}; // 将 Shard 0..3 分别绑定到物理核 2..5
    cfg.base.storage.directory = "./state/exchange";
    cfg.base.io_backend = nf::IoBackendConfig::epoll();
    cfg.base.idle_strategy = nf::IdleStrategy::hybrid_backoff;
    cfg.base.telemetry_path = "/dev/shm/exchange.nfxt"; // 4 个分片共享同一份 mmap 遥测文件

    auto engine = nf::ShardedEngine<fix::Schema, nf::Inline>::create(cfg).value();

    // 重要：不同分片上的回调会并发执行；同一会话内部的回调严格串行！
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
    // 默认自动轮询分配分片（也可显式指定分片下标 0..3）：
    for (int i = 0; i < 16; ++i) {
        (void)engine.add_acceptor({
            .name = "client-" + std::to_string(i),
            .listener = "main",
            .local_comp_id = "EXCH",
            .remote_comp_id = "BUY_" + std::to_string(i),
        });
    }

    // 启动 K-1 个分片线程，并在当前线程运行 Shard 0；任意线程调用 engine.request_stop() 退出
    return engine.run().ok() ? 0 : 1;
}
```

### 选型速查表

| 业务特征 | 推荐方案 | 原因 |
| :--- | :--- | :--- |
| 单通道、低频/Ping-Pong、追求首笔极限延迟 | `DirectInitiator::send_stream()`（单线程） | 单向延迟最低（**4.69 µs**）、零跨线程切换、二进制仅 ~336 KB |
| 单通道、单线程一次发出多笔连单 | `DirectInitiator::enqueue_stream()` + `flush()` | 合并为 1 次 `sendmsg()`（**1.27M ord/s**，CO-Free p99 19.8 µs） |
| 单通道、策略计算重 + 密集连发 | `DirectInitiator` + `DirectSendQueue`（双线程） | 策略计算与网络落盘双核并行（**1.43M ord/s**，CO-Free 延迟减半） |
| 2–8 个会话、回调快且无阻塞 | `Engine<Schema, Inline>`（`epoll` / `io_uring`） | 单协议线程无锁、应用状态天然单线程无需加锁 |
| 多会话、回调含数据库/RPC 阻塞 | `Engine<Schema, Queued>` | 慢回调隔离到工作池，不阻塞心跳与 TCP 收包 |
| 8–64+ 高并发服务端 | `ShardedEngine<Schema, Inline>`（`hybrid_backoff` 绑核） | 单端口多核线性扩展（**471k ord/s**，p99.9 降低 **7.8×**） |

---

## 4. 各使用模式下的详细线程模型

NimbleFIX 所有运行时均遵守同一核心不变量：**单写者协议归属（Single-Writer Protocol Ownership）**。任意一个 FIX 会话，在任意时刻都只由唯一一个操作系统线程（**协议归属线程**）修改其 `SessionCore`、序列号、TCP/TLS socket 与分段 mmap Journal —— 因此协议热路径上**零互斥锁**。

### 模式 1：`DirectInitiator`（纯单线程，零后台线程）

```text
[ 调用方 / 交易线程（协议归属线程） ]
   ├── send_stream() / enqueue_stream()+flush()  ──▶ mmap Journal ──▶ TcpTransport/TLS
   ├── poll_once() ──▶ 读 socket ──▶ 解析校验 ──▶ mmap Journal ──▶ Handler::on_message()（同步内联）
   ├── reconnect_if_due() ──▶ FailoverController 主备切换
   └── poll_maintenance() ──▶ 预分配/清理 Journal 分段
```

- **库创建的线程数**：**0**。
- **回调执行线程**：`Handler::on_message()` 在 `poll_once()` 内部同步执行。
- **规则**：禁止从多个线程并发调用同一个 `DirectInitiator` 实例的任何方法。

### 模式 2：`DirectInitiator` + `DirectSendQueue`（双线程无锁 SPSC）

```text
[ 策略线程（唯一生产者） ]                           [ I/O 线程（协议归属线程 / 唯一消费者） ]
   └── queue.try_push_stream<M>(fn)                    ├── init.drain(queue) ──▶ Journal ──▶ sendmsg()
             │                                         ├── init.poll_once() ──▶ Handler::on_message()
             ▼  （无锁 SPSC 环，acquire/release）       └── init.poll_maintenance()
     [ DirectSendQueue ] ────────────────────────────▶
```

- **库创建的线程数**：**0**（两个线程均由用户自行创建和绑核）。
- **回调执行线程**：入站 `Handler::on_message()` 运行在 **I/O 线程**（而非策略线程）。若策略线程需要消费成交回报，请在 `Handler::on_message()` 内通过你自己的 SPSC 队列或原子结构回传。
- **规则**：同一个 `DirectSendQueue` 严格只允许 **1 个生产者线程**调 `try_push*()`、**1 个消费者线程**调 `init.drain(queue)`。

### 模式 3：`Engine<Schema, Inline>`（单协议线程 + 可选外部发单线程）

```text
[ 外部业务线程（任意数量） ]
   ├── Sender::send_stream()        ──▶ (有界 MPSC 命令队列) ──────────┐
   ├── ProducerLane::send_stream()  ──▶ (无锁 SPSC 环，限 1 线程) ──────┼──▶ eventfd 唤醒
   └── engine.request_stop() / snapshot()                             │
                                                                      ▼
                                             [ 协议归属线程：engine.run() ]
                                                ├── IoBackend (epoll / busy_poll / io_uring)
                                                ├── 全部 SessionCore 与 mmap Journal（0 锁）
                                                └── 所有业务回调 on<M>() 在此线程同步执行
```

- **库创建的线程数**：**0**（`engine.run()` 直接在调用线程执行）。
- **回调执行线程**：全部回调（`on<M>()`、`on_message()`、`on_session_event()`、`on_logon_auth()`）都在 `engine.run()` 线程按顺序执行；仅在回调内访问的状态无需加锁。
- **规则**：`Inline` 回调必须快且非阻塞（切勿在回调内 `sleep` 或做慢速 DB/RPC，否则会阻塞同一 `Engine` 下所有会话的心跳与收包）。

### 模式 4：`Engine<Schema, Queued>`（单协议线程 + N 个工作线程）

```text
[ 协议归属线程：engine.run() ]                   [ 工作线程池：cfg.queued.worker_threads ]
   ├── Socket I/O、解析校验、心跳维护              ├── Worker 1 ──┐
   ├── 入站帧先落盘到 mmap Journal                ├── Worker 2 ──┼─▶ on<M>(ctx, view)
   └── 投入每会话串行队列 ───────────────────────▶└── Worker N ──┘     └── ctx.send_stream()（回投协议线程）
         （同一会话同时最多 1 个回调在途！）
```

- **库创建的线程数**：`cfg.queued.worker_threads` 个工作线程（默认 2 个）。
- **回调顺序与并发规则**：
  - **同一会话内**：严格串行（按 `MsgSeqNum` 顺序，同时最多 1 个回调在运行），但前后两次回调可能被调度到不同工作线程（库保证两者间的 happens-before 内存可见性）。
  - **不同会话间**：会在多个工作线程上**并行执行** —— 跨会话共享的应用状态必须自行同步。
- **认证回调**：`on_logon_auth()` 等协议关键钩子仍在协议归属线程执行。

### 模式 5：`ShardedEngine<Schema, Policy>`（K 个绑核分片 Reactor）

```text
                                 [ 共享监听端口（如 :9876） ]
                                              │
                                    (accept + Logon 35=A)
                                              ▼
       ┌───────────────────────────────[ Shard 0（调用线程，绑定核 c0） ]
       │      （通过无锁 fd 队列移交连接）       ├── 归属 Shard 0 的全部会话
       ├───────────────────────────┐           └── （若为 Queued 则含独立工作池 0）
       ▼                           ▼
[ Shard 1（线程 1，绑定核 c1） ]  [ Shard K-1（线程 K-1，绑定核 cK-1） ]
  ├── 归属 Shard 1 的全部会话       ├── 归属 Shard K-1 的全部会话
  └── （可选工作池 1）              └── （可选工作池 K-1）
```

- **库创建的线程数**：`K - 1` 个分片 Reactor 线程（Shard 0 在调用 `engine.run()` 的线程上运行）；若 `Policy == Queued`，每个分片再各自拥有 `worker_threads` 个工作线程。
- **回调并发规则**：
  - 每个会话静态绑定到唯一分片，同一会话的回调严格串行。
  - **不同分片上的不同会话会在不同 CPU 核上真正并行执行回调**！凡是跨分片共享的业务状态（如全局风控计数、共享订单簿）必须做好线程同步、原子操作或按分片分区（`thread_local`）。

### 各对象/句柄线程安全性速查

| 对象 / 句柄 | 线程安全性 | 使用约束 |
| :--- | :--- | :--- |
| `DirectInitiator` | **非线程安全**（单线程独占） | 全部方法只能由同一个 I/O 线程调用 |
| `DirectSendQueue` | **无锁 SPSC**（最多 2 线程） | 严格 1 个生产者线程（`try_push*`）+ 1 个消费者线程（`init.drain`） |
| `Engine` / `ShardedEngine` 配置期接口（`on`, `add_*`） | **仅限启动前调用** | 在 `run()` / `start()` 之前完成注册与添加 |
| `Engine::request_stop()`、`snapshot()`、`receipt()` | **线程安全** | 运行期间可由任意控制/监控线程安全调用 |
| `Sender<Schema>`（`handle.sender()`） | **线程安全（MPSC）** | 可在任意数量外部线程间并发发单 |
| `ProducerLane<Schema>`（`handle.producer_lane()`） | **无锁 SPSC**（单生产者线程） | 自动绑定首次调用线程；跨线程误用返回 `ErrorCode::wrong_thread` |
| `SessionContext` 及 `FlatMessageView` / `{Msg}View` | **仅当次回调有效** | 严禁把视图或 `SessionContext&` 保存到回调之外；如需留存必须拷贝数据 |
| `TelemetryReader` | **只读，跨进程安全** | 以 `O_RDONLY`/`PROT_READ` 映射 `.nfxt`，对交易进程零锁零干扰 |
| `JournalReplayer` | **在线/离线均安全** | 通过带租约保护的 `JournalReader` 读取，可与活跃写入进程安全并发 |

---

## 5. 职责边界矩阵：用户自行管理 vs 库自动管理

明确“库管什么、用户管什么”是线上零事故的关键：

| 领域 | **NimbleFIX 库自动管理** | **用户 / 应用程序必须自行管理** |
| :--- | :--- | :--- |
| **配置与密钥** | 校验全部 C++ 配置结构体（`EngineConfig`、`DirectInitiatorConfig`、`TlsConfig`、`ConnectionStrategy`）；持久化 Journal 时自动脱敏 Logon 密码。 | 选择配置文件格式（JSON/YAML/TOML/DB/环境变量）、解析配置、轮换密钥并填充到 C++ 结构体。 |
| **事件循环与线程** | `Engine::run()` / `ShardedEngine::run()` 驱动 `IoBackend`、定时器、分片线程、绑核（配置了 `cpu_core`/`cpu_cores` 时）与 `Queued` 线程池。 | 使用 `DirectInitiator`（或手动 `Engine::start()` + `poll_once()`）时，持续驱动 `poll_once()`、`drain()`、`reconnect_if_due()` 与 `poll_maintenance()`；自建策略/I/O 线程并绑核；捕获 `SIGINT`/`SIGTERM` 调用 `request_stop()`。 |
| **回调安全与生命周期** | 保证同一会话按 `MsgSeqNum` 严格有序投递，且零拷贝视图在回调期间始终有效。 | 若回调后仍需使用字段值须自行拷贝；在 `Queued` 或 `ShardedEngine` 下同步跨会话共享状态；保证 `Inline` 回调快且不阻塞。 |
| **崩溃恢复与幂等性** | 入站先落盘再回调，出站先落盘再发包；进程崩溃/`SIGKILL` 重启后自动重建序列号，重投未完成入站消息（`DeliveryInfo::is_recovery == true`），并复用已落盘的关联回复。 | 当 `info.is_recovery == true` 时，保证外部副作用（下游报单、写库、扣减风控额度）**幂等**（跨崩溃投递语义为至少一次 at-least-once）。 |
| **FIX 会话协议** | 完整实现 FIX 4.2/4.3/4.4/FIXT.1.1 状态机：Logon、Heartbeat、TestRequest、序列号追踪、`ResendRequest`、`GapFill`、业务 `Replay`（`43=Y` + `122=OrigSendingTime`）、`369=LastMsgSeqNumProcessed`、Session Reject (`35=3`)、优雅 Logout。 | 业务订单状态机、`BusinessMessageReject (35=j)` 业务拒绝规则、以及作为 Acceptor 时在 `on_logon_auth()` 中校验对端凭据。 |
| **TCP / TLS 与主备切换** | 非阻塞 TCP（`TCP_NODELAY`、`sendmsg` 批发送）、OpenSSL 3 TLS 1.2/1.3 + CA/SAN/mTLS + 重连复用 `TlsContext`、以及确定性多节点主备切换（`ordered`/`round_robin`、指数退避、重试上限）。 | 提供数字 IPv4/IPv6 地址（如需域名解析由应用层完成）、维护合法证书与私钥文件、以及配置了有限 `max_retries` 时处理 `FailoverController::exhausted()`。 |
| **系统时钟与 `SendingTime`** | 通过缓存 `FastUtcClock` 高速格式化 `52=SendingTime`（`ms`/`us`/`ns`）并校验对端时钟偏差（`max_sending_time_skew_ms`，默认 120 s）。 | 运维层面通过 NTP / PTP 保持宿主机系统时钟准确同步。 |
| **存储与持久性边界** | 预分配分段 `MAP_SHARED` mmap Journal、SSE4.2 硬件 CRC32C、`.tmp` + 原子 `rename` 建段、段轮转、读租约保护、满容 fail-closed 不覆盖未重传历史。 | 规划充足磁盘与容量预算（`capacity_bytes`、`segment_bytes`）、监控磁盘健康，并知晓 `appended` 保障**同机进程崩溃恢复**（热路径不做逐消息 `fsync`，不提供宿主机突然断电保护）。 |
| **可观测性与历史回放** | 配置 `telemetry_path` 后无锁写入会话计数与 5 阶段 HDR 延迟直方图（`read`/`parse`/`validate`/`journal`/`write`）；提供 `TelemetryReader` 与 `JournalReplayer`。 | 部署旁路采集进程/Prometheus Exporter（调用 `TelemetryReader` 或 `nimblefix telemetry`），以及按合规要求归档历史 Journal 文件。 |

---

## 6. 运维与进阶能力：TLS、主备切换、字典编译、旁路遥测与历史回放

### 6.1 开启 TLS / mTLS

- **Layer 0（`DirectInitiator`）**：链接 `NimbleFIX::nimblefix_core_tls` 并替换 `Transport` 模板参数为 `nf::DirectTlsTransport`：
  ```cpp
  nf::TlsConfig tls{};
  tls.enabled = true;
  tls.ca_file = "/etc/certs/ca.pem";
  tls.server_name = "fix.venue.example";
  tls.minimum_version = 13; // TLS 1.3（或 12 表示 TLS 1.2+）
  // 双向 mTLS 额外补充两行：
  // tls.certificate_file = "/etc/certs/client.pem";
  // tls.private_key_file = "/etc/certs/client.key";

  auto transport = nf::DirectTlsTransport::create(std::move(tls)).value();
  auto init = nf::DirectInitiator<fix::Schema, TradingHandler, nf::DirectTlsTransport>::open(
      std::move(cfg), TradingHandler{}, std::move(transport)).value();
  ```
- **Layer 1/2（`Engine` / `ShardedEngine`）**：直接配置 `ListenerConfig::tls`（服务端）或 `InitiatorSessionConfig::tls`（客户端）。

### 6.2 多节点主备自动切换（`ConnectionStrategy`）

`DirectInitiatorConfig` 与 `InitiatorSessionConfig` 接口完全一致：

```cpp
cfg.endpoints = {
    {"10.0.1.10", 9876}, // 主节点
    {"10.0.2.10", 9876}, // 备节点 / 灾备中心
};
// 主节点优先：主连失败立即（0 ms）切备；一轮全失败后 250ms -> 500ms -> ... -> 10s 指数退避；
// 成功建链并断线后自动回归主节点：
cfg.connection_strategy = nf::ConnectionStrategy::ordered(
    /*initial_ms=*/250, /*max_ms=*/10'000, /*multiplier=*/2,
    /*retries=*/0, /*reset_primary=*/true, /*immediate=*/true);
// 或轮询负载均衡：nf::ConnectionStrategy::round_robin(250, 10'000, 2);
```

### 6.3 纯 C++20 字典编译器（`nimblefix-schema-gen`）

```sh
# 1. 生成全量 FIX 4.4 头文件：
nimblefix-schema-gen --output generated/fix44.hpp --namespace my::fix44 schemas/standard/FIX44.xml

# 2. 按业务消息子集裁剪（大幅加速编译并减小二进制体积）：
nimblefix-schema-gen --output generated/venue.hpp --namespace my::venue \
  --messages NewOrderSingle,OrderCancelRequest,ExecutionReport schemas/standard/FIX44.xml

# 3. 从真实 SOH / 竖线('|') / .nfxj 日志自动裁剪未出现过的消息与可选字段：
nimblefix-schema-gen --output generated/venue.hpp --namespace my::venue \
  --from-fix-log ./captures/session.log schemas/standard/FIX44.xml
```

### 6.4 旁路 mmap 遥测（`TelemetryReader`）

在 `DirectInitiatorConfig` 或 `EngineConfig` 中开启 `telemetry_path = "/dev/shm/app.nfxt"` 与 `telemetry_stage_probes = true` 后，可在独立监控进程中零干扰读取实时指标与分阶段 p99/p99.99 延迟：

```cpp
#include <iostream>
#include <nimblefix/telemetry.hpp>

auto reader = nimblefix::TelemetryReader::open("/dev/shm/app.nfxt").value();
for (const auto& s : reader.snapshot()) {
    const auto parse_lat = s.stage_latency(nimblefix::TelemetryStage::parse);
    std::cout << s.session_key << " rx=" << s.rx_messages
              << " tx=" << s.tx_messages
              << " parse_p99_ns=" << parse_lat.p99_ns << '\n';
}
// 或直接输出 Prometheus 文本格式：std::cout << reader.to_prometheus();
```

### 6.5 确定性历史回放（`JournalReplayer`）

```cpp
#include <nimblefix/replay.hpp>

nimblefix::ReplayOptions opts{};
opts.journal_path = "./state/buy/FIX.4.4_BUY_SELL.nfxj";
opts.direction = nimblefix::ReplayDirection::inbound;
opts.timing = nimblefix::ReplayTiming::scaled;
opts.speed_multiplier = 10.0; // 按原始 52=SendingTime 间隔 10 倍速回放
opts.skip_admin = true;

// 方式 1：进程内回调接收（离线回测 / 故障复现）
auto stats = nimblefix::JournalReplayer::replay(opts,
    [](std::span<const char> raw_fix, const nimblefix::JournalRecordHeader& hdr) {
        (void)raw_fix; (void)hdr;
        return nimblefix::Status::success();
    }).value();

// 方式 2：原样通过 TCP 灌入测试环境 Acceptor
// auto stats = nimblefix::JournalReplayer::replay_to_tcp(opts, {"127.0.0.1", 9876}).value();
```

### 6.6 可选独立 CLI 工具（`nimblefix`）

若编译时开启 `-DNIMBLEFIX_BUILD_CLI=ON`（或 `--cli=y`），可选独立命令行程序 `nimblefix` 提供了基于 TOML 的应用层运维示例：

```sh
nimblefix doctor --json
nimblefix init --directory ./demo
nimblefix validate --config ./demo/acceptor.toml --json
nimblefix run --config ./demo/acceptor.toml
nimblefix telemetry --path ./demo/telemetry.nfxt --prometheus
nimblefix replay --path ./demo/state/.../session.nfxj --direction inbound --timing original --endpoint 127.0.0.1:9876
nimblefix schema generate --xml schemas/standard/FIX44.xml --namespace venue --output venue.hpp
```
