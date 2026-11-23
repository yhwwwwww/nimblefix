# NimbleFIX 0.3 快速入门

NimbleFIX 0.3 是一个追求**极低延迟、崩溃可恢复、接口克制**的 C++20 FIX 引擎，按**用多少付多少**分成三层：

| 你的场景 | 选哪一层 | 链接目标 (CMake / xmake) | 额外依赖 |
| :--- | :--- | :--- | :--- |
| **极速单连接交易客户端**（策略 / 报单直连） | **Layer 0 `DirectInitiator`** | `NimbleFIX::nimblefix_core` / `nimblefix-core` | 仅 C++20 + `pthread` |
| **极速单连接 + TLS / mTLS** | **Layer 0 + TLS** | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | 系统 OpenSSL 3 |
| **多会话服务端（Acceptor）/ 网关 / CLI** | **Layer 1/2 `Engine`** | `NimbleFIX::nimblefix` / `nimblefix` | 可选 `liburing`、`libxml2` |

> 想了解协议支持边界与资源上限，可查阅 [session-support.md](session-support.md)。

---

## 一、5 分钟跑通极速 Initiator（`DirectInitiator`）

`DirectInitiator` 是单线程、非阻塞、**热路径 0 堆分配 / 0 锁 / 0 fsync** 的轻量客户端，内置 v2 分段 mmap Journal，自动负责断线重连后的序列号恢复、`ResendRequest`、`GapFill` 与业务重放。

### 1. 最小可用代码（只要 5 个必填字段）

```cpp
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44.hpp>

namespace nf = nimblefix;
namespace fix = nimblefix::fix44;

// 1) 定义回调处理器（无需继承，无需 std::function）
struct MyHandler {
    nf::Status on_message(const nf::FlatMessageView& msg,
                          const nf::DeliveryInfo& info) noexcept {
        // info.is_recovery == true 表示这是进程重启后恢复重投的消息
        if (auto mt = msg.find_unique(35); mt.ok() && mt.value().value == "8") {
            // 收到 ExecutionReport
        }
        return nf::Status::success();
    }
};

int main() {
    // 2) 打开会话（session_key 省略时会自动按 FIX.4.4/BUY/SELL 生成）
    auto init = nf::DirectInitiator<fix::Schema, MyHandler>::open({
        .host = "127.0.0.1",
        .port = 9876,
        .storage_directory = "./state/buy",
        .local_comp_id = "BUY",
        .remote_comp_id = "SELL",
    }).value();

    (void)init.connect();
    while (!init.is_active()) (void)init.poll_once();

    // 3) 发单（日常写法：简洁易读；极速零分配写法见下方）
    auto order = fix::NewOrderSingle::make()
        .cl_ord_id("ORD-001")
        .symbol("AAPL")
        .side('1')
        .transact_time("20261123-09:30:00.000")
        .order_qty(nf::Decimal{100, 0})
        .ord_type('1');

    auto seq = init.send(order); // 直接写入 mmap Journal 并发送

    // 4) 持续推进收发与心跳
    while (init.valid_transport()) (void)init.poll_once();
}
```

### 2. 开启 TLS / mTLS（仅换一个模板参数）

链接 `NimbleFIX::nimblefix_core_tls`（xmake 下为 `nimblefix-core-tls`），无需改任何业务代码：

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

### 3. 追求极致零分配发单（`FixedMessage`）

上面的 `make()` 上手最简单；如果要在微秒级策略循环里做到**全程 0 `malloc`**，把复用对象换成栈上的 `FixedMessage`（见完整示例 [examples/direct_initiator.cpp](../examples/direct_initiator.cpp)）：

```cpp
using OrderShape = nf::Fields<
    nf::Field<11, nf::FieldKind::string>,    // ClOrdID
    nf::Field<55, nf::FieldKind::string>,    // Symbol
    nf::Field<54, nf::FieldKind::character>, // Side
    nf::Field<60, nf::FieldKind::string>,    // TransactTime
    nf::Field<38, nf::FieldKind::decimal>,   // OrderQty
    nf::Field<40, nf::FieldKind::character>  // OrdType
>;

fix::NewOrderSingleFixed<OrderShape, 128> order; // 栈上复用，零堆分配
order.reset();
(void)order.cl_ord_id("ORD-001");
(void)order.symbol("AAPL");
(void)order.side('1');
(void)order.transact_time("20261123-09:30:00.000");
(void)order.order_qty(nf::Decimal{100, 0});
(void)order.ord_type('1');
(void)init.send(order);
```

---

## 二、多会话服务端（Acceptor）与可插拔 `IoBackend`

需要监听端口、管理多个会话、从业务线程并发下单，或者使用 `Queued` 异步业务线程池时，使用 `Engine<Schema, Policy>`：

```cpp
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>

namespace nf = nimblefix;
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

    (void)engine.run(); // 在当前线程运行事件循环；其他线程可调用 engine.request_stop()
}
```

### `IoBackend` 怎么选？

| 后端 | C++ 配置 | TOML 配置 (`[engine]`) | 适用场景 |
| :--- | :--- | :--- | :--- |
| **`epoll`**（默认） | `IoBackendConfig::epoll()` | `io_backend = "epoll"` | 通用默认，零额外依赖 |
| **`busy_poll`** | `IoBackendConfig::busy_poll()` | `io_backend = "busy_poll"` | 极少连接 + 独占隔离核，追求最低单连接延迟 |
| **`io_uring`** | `IoBackendConfig::io_uring()` | `io_backend = "io_uring"` | 多连接高并发（Proactor 批处理 + 空闲休眠，吞吐最高、p99 最稳；需开启 `--io_uring=y` / `-DNIMBLEFIX_ENABLE_IO_URING=ON`） |

> 未开启 `io_uring` 编译选项时，`nf::io_uring_available()` 返回 `false`，请求 `io_uring` 会安全返回 `ErrorCode::unsupported`。

### 跨线程发单与线程规则（只需记住 3 条）

1. **会话归属**：`engine.run()` 所在线程拥有所有协议状态；若需嵌入外部事件循环，改用 `engine.start()` + 同线程循环调用 `engine.poll_once(max_wait_ms)`。
2. **其他线程发单**：启动前通过 `auto sender = handle.sender<fix::Schema>();` 取出 `Sender`，可在任意业务线程安全并发 `sender.send(msg)`（极致单生产者场景可用 `handle.producer_lane<fix::Schema>()`）。
3. **回调生命周期**：回调里的 `View` 和 `SessionContext` 仅在当次回调内有效，不要把它们保存到其他线程使用。

---

## 三、动手运行自带示例与 CLI

### 1. 本机双端订单演示（[acceptor.cpp](../examples/acceptor.cpp) + [initiator.cpp](../examples/initiator.cpp)）

```sh
xmake f -m release --tls=y --xml=y --io_uring=y
xmake build acceptor-example initiator-example direct-initiator-example
```

分别在两个终端启动：

```sh
# 终端 1（服务端）
xmake run acceptor-example --state ./state/sell --port 9876

# 终端 2（客户端，看到 SESSION_ACTIVE 后即可输入命令）
xmake run initiator-example --state ./state/buy --port 9876
```

在客户端输入：

```text
order ORDER-001 AAPL buy 100 150.25
order ORDER-002 MSFT sell 25 market
status
quit
```

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

> TLS 与认证同样可在 TOML 中通过 `[listeners.tls]`、`[sessions.tls]`、`[sessions.authentication]`（`password_env` 引用环境变量，绝不把密码写入日志或 Journal）配置。

---

## 四、自定义字典、超大 Tag 与子集代码生成

- **子集裁剪（大幅加速编译、缩小体积）**：用 `--messages` 只生成你实际交易的业务消息（管理消息与依赖的嵌套重复组会自动保留）：

  ```sh
  python3 tools/schema/generate.py --output generated/venue.hpp --namespace venue \
    --messages NewOrderSingle,OrderCancelRequest,ExecutionReport schemas/standard/FIX44.xml
  ```

- **任意自定义 Tag 编号**：无论是 `8899` 还是 `900001`，全链路原生支持 `1..999,999,999` 整个 FIX Tag 范围，内部全部按紧凑成员下标索引，**不存在 1024-bit 上限或内存膨胀**。
- **运行期未知 Tag**：如需按字符串动态拼接运行期才确定的 Tag，使用 `DynamicMessage` 即可。

---

## 五、持久化、恢复与基准测试

- **统一 v2 分段 mmap Journal**：全引擎（含 `DirectInitiator` 和 `Engine`）统一使用带硬件 SSE4.2 CRC32C 校验的分段 mmap Journal；入站先落盘再投递，出站先落盘再写 socket。
- **崩溃恢复承诺**：进程异常退出或 `SIGKILL` 后重启，自动恢复序列号、重投未完成的入站消息（`is_recovery == true`），并完整服务对手方的 `ResendRequest` / `GapFill` / `Replay`。热路径不做逐消息 `fsync`/`msync`（保障同机进程崩溃恢复，不承诺整机断电落盘）。
- **构建、测试与基准**：

```sh
# 运行全量测试
xmake f -m release --tls=y --xml=y --benchmarks=y -y
xmake check

# 运行 Initiator 基准（Pre-TCP + 真实 TCP 环回）
xmake build direct-initiator-bench
./build/linux/x86_64/release/direct-initiator-bench 5000 30000

# 运行 Acceptor + 三大 IoBackend 对比基准（epoll / busy_poll / io_uring）
xmake f -m release --tls=y --xml=y --io_uring=y --benchmarks=y -y
xmake build acceptor-bench
./build/linux/x86_64/release/acceptor-bench 2000 20000 8 all
```

- **外部 CMake 项目接入**：

```cmake
find_package(NimbleFIX 0.3 CONFIG REQUIRED COMPONENTS core)
# 只要极速 Initiator：
target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core)
# 极速 Initiator + TLS：
# target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core_tls)
# 完整多会话 Engine / Acceptor：
# target_link_libraries(my_gateway PRIVATE NimbleFIX::nimblefix)
```
