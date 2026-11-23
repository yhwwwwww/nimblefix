# NimbleFIX 0.3 快速入门

NimbleFIX 0.3 按**用多少付多少**分成三层，全部接口统一使用字段名（无数字 tag 魔数）：

| 你的场景 | 选哪一层 | 链接目标 (CMake / xmake) | 额外依赖 |
| :--- | :--- | :--- | :--- |
| **极速单连接交易客户端**（策略 / 报单直连） | **Layer 0 `DirectInitiator`** | `NimbleFIX::nimblefix_core` / `nimblefix-core` | 仅 C++20 + `pthread` |
| **极速单连接 + TLS / mTLS** | **Layer 0 + TLS** | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | 系统 OpenSSL 3 |
| **多会话服务端（Acceptor）/ 网关 / CLI** | **Layer 1/2 `Engine`** | `NimbleFIX::nimblefix` / `nimblefix` | 可选 `liburing`、`libxml2` |

> 架构设计见 [architecture.md](architecture.md) · 协议细节与资源边界见 [session-support.md](session-support.md) · 完整实测基准见 [benchmarks.md](benchmarks.md)。

---

## 一、先理清两个独立概念：**怎么构造消息** vs **怎么发送消息**

初学者最容易混淆“消息的构造方式”和“网络发送方式”，其实它们完全正交、可以自由组合：

### 1. 怎么构造一条消息（两种风格，按写法习惯二选一）

两种写法默认都在栈上内置 **16 字段槽 + 256B 字符串区（SSO）**，常规订单均为 **0 `malloc`**；遇到偶发超多字段或超长字符串时**自动溢到堆上，绝不拒单**。如果需要更大的栈上免分配阈值，两者都支持直接自定义 `<槽位数, 字节数>`。

**它们的核心区别在于“一次性链式表达式”还是“可逐句修改的普通对象”：**

| 对比维度 | 写法 A：`NewOrderSingle::make()` | 写法 B：`NewOrderSingle::Draft` |
| :--- | :--- | :--- |
| **对象性质** | **不可变链式构造器（右值消费）**——必须一条链拼完 | **常规可变对象**——先声明变量，再逐句赋值 |
| **能否拆开写 `order.symbol(...)`** | **不能**（每次 setter 返回新类型，不能对左值变量继续追加字段） | **可以**，逐句写、链式写都行 |
| **如何写条件可选字段** | 传 `std::optional`（`nullopt` 自动跳过） | 原生 `if (...) order.price(...)` 和 `std::optional` 都可以 |
| **覆盖已填字段 / `reset()` 复用** | **不支持**（一次性构造即用） | **支持**（同名字段直接覆盖，循环里 `order.reset()` 原地复用） |
| **自定义 SSO 容量** | `NewOrderSingle::make<32, 1024>()`（默认 `<16, 256>`） | `NewOrderSingle::DraftWith<32, 1024>`（默认 `<16, 256>`） |
| **什么时候选它** | 一行链式写完、字段来源直接就是结构体/optional（编译期定序，无运行期排序） | 业务里有多条 `if` 分段拼装、需要改写字段，或在热循环里 `reset()` 复用 |

```cpp
namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

// 写法 A：一次性链式 make()（必须一气呵成拼完；可选字段传 std::optional）
std::optional<nf::Decimal> opt_price = is_limit ? std::optional{nf::Decimal{15025, 2}} : std::nullopt;
auto order_a = fix::NewOrderSingle::make() // 如需更大栈缓冲：make<32, 1024>()
    .cl_ord_id("ORD-001")
    .symbol("AAPL")
    .side('1')
    .transact_time("20261123-09:30:00.000")
    .order_qty(nf::Decimal{100, 0})
    .ord_type(is_limit ? '2' : '1')
    .price(opt_price);                     // 有值就编 44=...，nullopt 自动跳过
// 注意：下面这样对已有变量追加字段在写法 A 上是不允许的：
// order_a.price(...);  // ❌ 编译不通过；需要逐句赋值/if 请用写法 B

// 写法 B：普通可变对象 Draft（支持逐句赋值、原生 if、覆盖改写、reset() 复用）
fix::NewOrderSingle::Draft order_b;        // 如需更大栈缓冲：DraftWith<32, 1024>
order_b.cl_ord_id("ORD-001");
order_b.symbol("AAPL");
order_b.side('1');
order_b.transact_time("20261123-09:30:00.000");
order_b.order_qty(nf::Decimal{100, 0});
order_b.ord_type(is_limit ? '2' : '1');
if (is_limit) order_b.price(nf::Decimal{15025, 2}); // 原生 if，随便写
// 下一笔直接原地复用：
// order_b.reset();
```

> 无论用写法 A 还是写法 B 构造的消息，都可以直接传给下面任意一种发送方式。

---

### 2. `DirectInitiator` 怎么发送消息（3 种发送模式）

| 发送模式 | API | 适用场景 | 特点 |
| :--- | :--- | :--- | :--- |
| **① 单线程直发** | `init.send(order)` | 单笔低频 / Ping-Pong / 追求首笔最低延迟 | 立刻编码进 mmap Journal 并做 1 次非阻塞 `send()`，零跨线程开销 |
| **② 同线程攒批发送** | `init.enqueue(order)` + `init.flush()` | 同线程密集连发（Burst，如一次扫多档发 10 笔） | 多笔订单先写入 Journal，最后 `flush()` 自动用 **1 次 `sendmsg/writev`** 整批发出 |
| **③ 双线程无锁 SPSC 队列** | `DirectSendQueue`: 策略线程 `queue.try_push(order)` + I/O 线程 `init.drain(queue)` | 策略计算重 + 密集发单，需要策略核与网络核并行流水线 | 0 锁 0 `malloc`；策略线程把字段编码进无锁环，I/O 线程批量取单、编序列号落盘并 **1 次 `sendmsg/writev`** 发出 |

#### 实测速查：不同场景下 4 种发单方式怎么选？（Release, FIX 4.4 + mmap Journal, 0 `malloc`）

| 发送模式 | Ping-Pong 单笔单向 p50 | 纯密集连发 (32 笔×0 µs) CO-Free p50 / 吞吐 | 策略计算+连发 (32 笔×2 µs) CO-Free p50 / p99 |
| :--- | ---: | ---: | ---: |
| **① `DirectInitiator::send()`** | **4.97 µs** | 57.04 µs / 292k ord/s | 100.50 µs / 198.74 µs |
| **② `DirectInitiator enqueue+flush`** | 5.01 µs | 22.73 µs / 946k ord/s | 95.40 µs / 130.63 µs |
| **③ `DirectInitiator + DirectSendQueue`** | 5.13 µs | **18.14 µs** / **1.02M ord/s** | **48.52 µs** / **88.70 µs** |
| **④ `Engine(busy_poll) + ProducerLane`** | 7.89 µs | 76.31 µs / 249k ord/s | 79.85 µs / 152.32 µs |

> `CO-Free` = `对端收到时间 - Burst 开始时间`（无 Coordinated Omission 的真实延迟）。完整数据与构造方法（`make()` 340 ns / `Draft` 321 ns / `Fixed` 221 ns，均 0 分配）见 [benchmarks.md](benchmarks.md)。

---

## 二、5 分钟跑通极速 Initiator（`DirectInitiator`）

### 1. 最小完整示例（单线程直发 + 强类型收回报）

```cpp
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

// 收消息回调：全强类型字段名，无数字 tag
struct MyHandler {
    nf::Status on_message(const nf::FlatMessageView& msg,
                          const nf::DeliveryInfo& info) noexcept {
        if (msg.is<fix::ExecutionReport>()) {
            const auto er = msg.as<fix::ExecutionReport>();
            // info.is_recovery == true 表示进程崩溃重启后恢复重投的消息
            auto exec_id = er.exec_id();
            auto symbol  = er.symbol();
            (void)exec_id; (void)symbol; (void)info;
        }
        return nf::Status::success();
    }
};

int main() {
    // 打开会话（只要 5 个必填字段；session_key 省略时自动生成为 FIX.4.4/BUY/SELL）
    auto init = nf::DirectInitiator<fix::Schema, MyHandler>::open({
        .host = "127.0.0.1",
        .port = 9876,
        .storage_directory = "./state/buy",
        .local_comp_id = "BUY",
        .remote_comp_id = "SELL",
    }).value();

    (void)init.connect();
    while (!init.is_active()) (void)init.poll_once();

    // 构造消息（用写法 A 的 make() 或写法 B 的 Draft 都可以，默认 SSO 0 分配）
    auto order = fix::NewOrderSingle::make()
        .cl_ord_id("ORD-001").symbol("AAPL").side('1')
        .transact_time("20261123-09:30:00.000")
        .order_qty(nf::Decimal{100, 0}).ord_type('1');

    // 模式 ①：单线程直发（立即写 Journal 并发出）
    auto seq = init.send(order);
    (void)seq;

    // 持续推进收发与心跳
    while (init.valid_transport()) (void)init.poll_once();
}
```

### 2. 切换到同线程攒批（模式 ②）或双线程无锁队列（模式 ③）

```cpp
// 模式 ②：同线程密集连发 10 笔，合并为 1 次 writev/sendmsg 系统调用
for (int i = 0; i < 10; ++i) {
    (void)init.enqueue(order);
}
(void)init.flush();

// 模式 ③：双线程流水线（策略线程生产 + I/O 线程消费）
nf::DirectSendQueue queue{256, 512}; // 256 槽位，每条最大 512 字节，无锁 SPSC

// 策略线程：
(void)queue.try_push(order);         // 0 锁 0 malloc，把业务字段写入 SPSC 环

// I/O 线程循环中：
(void)init.drain(queue);             // 批量弹出 -> 写 Journal -> 1 次 writev 发出
(void)init.poll_once();              // 读回包 + 心跳
```

### 3. 开启 TLS / mTLS（只换一个模板参数）

链接 `NimbleFIX::nimblefix_core_tls`（xmake 下为 `nimblefix-core-tls`），业务代码零改动：

```cpp
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/direct_tls_transport.hpp>

nf::TlsConfig tls{};
tls.enabled = true;
tls.ca_file = "/path/to/ca.pem";
tls.server_name = "fix.venue.example";
// 如需双向 mTLS，再补两行：
// tls.certificate_file = "/path/to/client.pem";
// tls.private_key_file = "/path/to/client.key";

auto transport = nf::DirectTlsTransport::create(std::move(tls)).value();
auto init = nf::DirectInitiator<fix::Schema, MyHandler, nf::DirectTlsTransport>::open(
    std::move(cfg), MyHandler{}, std::move(transport)).value();
```

---

## 三、多会话服务端（Acceptor）与可插拔 `IoBackend`

需要监听端口、管理多个会话、多线程并发发单或使用 `Queued` 异步业务线程池时，使用 `Engine<Schema, Policy>`：

```cpp
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

int main() {
    nf::EngineConfig cfg{};
    cfg.storage.directory = "./state/sell";
    // 一行切换 I/O 后端：epoll()（默认）| busy_poll() | io_uring()
    cfg.io_backend = nf::IoBackendConfig::epoll();

    // Policy: nf::Inline（默认，最低延迟）或 nf::Queued（业务回调隔离到线程池）
    auto engine = nf::Engine<fix::Schema, nf::Inline>::create(cfg).value();

    (void)engine.on<fix::NewOrderSingle>(
        [](nf::SessionContext& ctx, const fix::NewOrderSingleView& req) {
            auto er = fix::ExecutionReport::make()
                .order_id("ORD-1").exec_id("EX-1")
                .cl_ord_id(req.cl_ord_id().value())
                .exec_type('0').ord_status('0')
                .symbol(req.symbol().value()).side(req.side().value())
                .leaves_qty(req.order_qty().value())
                .cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
            return ctx.send(er).status();
        });

    (void)engine.add_listener({.name = "main", .endpoint = {"127.0.0.1", 9876}});
    (void)engine.add_acceptor({.name = "client-1", .listener = "main",
                               .local_comp_id = "SELL", .remote_comp_id = "BUY"});

    (void)engine.run(); // 在当前线程运行；其他线程可调用 engine.request_stop()
}
```

### `IoBackend` 怎么选？

| 后端 | C++ 配置 | TOML 配置 (`[engine]`) | 适用场景 |
| :--- | :--- | :--- | :--- |
| **`epoll`**（默认） | `IoBackendConfig::epoll()` | `io_backend = "epoll"` | 通用默认，零额外依赖 |
| **`busy_poll`** | `IoBackendConfig::busy_poll()` | `io_backend = "busy_poll"` | 极少连接 + 独占隔离核，追求最低单连接延迟 |
| **`io_uring`** | `IoBackendConfig::io_uring()` | `io_backend = "io_uring"` | 多连接高并发（Proactor 批处理 + 空闲休眠，吞吐最高、p99 最稳；需开启 `--io_uring=y` / `-DNIMBLEFIX_ENABLE_IO_URING=ON`） |

> 未开启 `io_uring` 编译选项时，`nf::io_uring_available()` 返回 `false`，请求 `io_uring` 会返回 `ErrorCode::unsupported`。

### `Engine` 跨线程发单（只需记住 3 条）

1. **会话归属**：`engine.run()` 所在线程拥有所有协议状态；若嵌入外部事件循环，改用 `engine.start()` + 同线程循环 `engine.poll_once(max_wait_ms)`。
2. **其他线程发单**：启动前通过 `auto sender = handle.sender<fix::Schema>();` 取出 `Sender`，可在任意线程安全并发 `sender.send(msg)`；单生产者极限低延迟可用 `handle.producer_lane<fix::Schema>()`（无锁 SPSC）。
3. **回调生命周期**：回调里的 `View` 和 `SessionContext` 仅在当次回调内有效，不要存到其他线程使用。

---

## 四、动手运行自带示例与 CLI

### 1. 本机双端订单演示（[acceptor.cpp](../examples/acceptor.cpp) + [initiator.cpp](../examples/initiator.cpp)）

```sh
xmake f -m release --tls=y --xml=y --io_uring=y
xmake build acceptor-example initiator-example direct-initiator-example
```

分别在两个终端运行：

```sh
# 终端 1（服务端）
xmake run acceptor-example --state ./state/sell --port 9876

# 终端 2（客户端，看到 SESSION_ACTIVE 后输入命令）
xmake run initiator-example --state ./state/buy --port 9876
```

客户端可输入：`order ORDER-001 AAPL buy 100 150.25`、`order ORDER-002 MSFT sell 25 market`、`status`、`quit`。

### 2. 零代码用 TOML + CLI 跑通会话

```sh
xmake build nimblefix-cli
xmake run nimblefix-cli doctor --json                       # 查看当前构建已启用的能力
xmake run nimblefix-cli init --directory ./demo             # 生成可运行的 acceptor/initiator.toml
xmake run nimblefix-cli validate --config ./demo/acceptor.toml --json
xmake run nimblefix-cli run --config ./demo/acceptor.toml   # 终端 1
xmake run nimblefix-cli run --config ./demo/initiator.toml  # 终端 2
```

精简 TOML 配置示例：

```toml
version = 1

[engine]
dispatch = "inline"     # "inline"（默认）| "queued"
io_backend = "epoll"    # "epoll"（默认）| "busy_poll" | "io_uring"
schema = "fix44"        # 或 "none" 走无字典 flat 模式

[engine.storage]
directory = "./state/sell"
segment_bytes = 1048576
capacity_bytes = 16777216

[[listeners]]
name = "main"
endpoint = "127.0.0.1:9876"

[[sessions]]
name = "buy-side"
role = "acceptor"
listener = "main"
transport = "FIX.4.4"
local_comp_id = "SELL"
remote_comp_id = "BUY"
```

---

## 五、自定义字典、超大 Tag 与子集代码生成

- **子集裁剪（加速编译、缩小体积）**：用 `--messages` 只生成实际用到的业务消息（管理消息与依赖组自动保留）：

  ```sh
  python3 tools/schema/generate.py --output generated/venue.hpp --namespace venue \
    --messages NewOrderSingle,OrderCancelRequest,ExecutionReport schemas/standard/FIX44.xml
  ```

- **任意自定义 Tag**：原生支持 `1..999,999,999` 整个 FIX Tag 范围（如 `8899`、`900001`），全部按紧凑成员下标索引，无 1024-bit 上限或内存膨胀。
- **运行期才确定的未知 Tag**：使用 `DynamicMessage` 追加即可。

---

## 六、持久化、崩溃恢复与 CMake 接入

- **统一 v2 分段 mmap Journal**：全引擎统一使用带硬件 SSE4.2 CRC32C 的分段 mmap Journal，入站先落盘再投递，出站先落盘再写 socket。
- **崩溃恢复**：进程异常退出或 `SIGKILL` 重启后，自动恢复序列号、重投未完成入站消息（`is_recovery == true`），完整支持 `ResendRequest` / `GapFill` / `Replay`（热路径不做逐消息 `fsync`/`msync`，保障同机进程崩溃恢复）。
- **外部 CMake 项目接入**：

```cmake
find_package(NimbleFIX 0.3 CONFIG REQUIRED COMPONENTS core)
# 只要极速 Initiator（含 DirectSendQueue）：
target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core)
# 极速 Initiator + TLS：
# target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core_tls)
# 完整多会话 Engine / Acceptor：
# target_link_libraries(my_gateway PRIVATE NimbleFIX::nimblefix)
```
