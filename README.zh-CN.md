# NimbleFIX

[English](README.md) | **简体中文**

一个面向低延迟交易系统的现代 C++20 FIX 协议库，支持 FIX 4.2、4.3、4.4 与 FIXT 1.1 / 5.0 SP2。围绕确定性的亚微秒热路径设计，实现稳态零堆分配、分段 mmap 持久化与崩溃自动恢复。

[使用手册](docs/user-guide.zh-CN.md) · [架构设计](docs/architecture.zh-CN.md) · [支持边界矩阵](docs/session-support.zh-CN.md) · [性能基准](docs/benchmarks.zh-CN.md)

---

## 核心特性

- **按需分层链接**：可只链接轻量的单会话客户端核心（`NimbleFIX::nimblefix_core`），按需叠加 OpenSSL 3 TLS（`NimbleFIX::nimblefix_core_tls`），或扩展至多会话网关与多核分片服务端（`NimbleFIX::nimblefix`）。
- **零堆分配流式编解码**：使用 `nimblefix-schema-gen` 将标准或自定义 FIX XML 字典编译为强类型 C++20 头文件。发送时直接流式写入目标缓冲或 mmap 日志，原生支持 `if`/`for`、`std::optional` 与多层嵌套重复组，无中间消息对象。
- **内置持久化与崩溃恢复**：每个会话使用带硬件 SSE4.2 CRC32C 校验的预分配分段 mmap 日志。入站消息先落盘再投递、出站消息先落盘再发送，进程重启后自动恢复序列号并完整支持 `ResendRequest`、`GapFill` 与业务重传。
- **可扩展的 I/O 与多核运行时**：支持 `epoll`、`busy_poll` 及可选 Linux `io_uring` 后端；`ShardedEngine` 支持单端口接入并将各会话固定到绑定 CPU 核上并行处理。
- **开箱即用的运维能力**：支持多节点主备自动切换（`ConnectionStrategy`）、旁路 mmap 遥测与纳秒级 HDR 延迟直方图、确定性历史日志回放以及可选的 `nimblefix` 命令行工具。

---

## 库目标与分层

| 层级 | CMake / xmake 目标 | 包含能力 | 外部依赖 |
| :--- | :--- | :--- | :--- |
| **Core（核心层）** | `NimbleFIX::nimblefix_core` / `nimblefix-core` | 流式编码器（`StreamWriter`）、`Fixed<...>`、分段 mmap 日志、`SessionCore`、非阻塞 TCP（`sendmsg` 批发送）、`DirectInitiator`、多节点主备切换、无锁 SPSC `DirectSendQueue`、mmap 遥测、`JournalReplayer` | C++20 + `pthread` |
| **Core + TLS** | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | Core 全部能力 + 非阻塞 OpenSSL 3 `DirectTlsTransport`（TLS 1.2/1.3、CA/SAN 校验、双向 mTLS） | OpenSSL 3+ |
| **Full Runtime（完整运行时）** | `NimbleFIX::nimblefix` / `nimblefix` | 以上全部能力 + `Engine<Schema, Inline\|Queued>`、多核 `ShardedEngine`、Acceptor、可插拔 `IoBackend`（`epoll` / `busy_poll` / `io_uring`）、MPSC `Sender`、SPSC `ProducerLane`、可选运行期 XML（`NimbleFIX::nimblefix_xml`） | 可选 `liburing`、`libxml2` |

---

## 快速示例：低延迟客户端（Initiator）

完整可运行示例见 [examples/direct_initiator.cpp](examples/direct_initiator.cpp)；多会话网关、多核服务端、详细线程模型与职责划分请参阅 [使用手册](docs/user-guide.zh-CN.md)。

```cpp
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44_sample.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44_sample;

struct MyHandler {
    [[nodiscard]] nf::Status on_message(const nf::FlatMessageView& msg,
                                        const nf::DeliveryInfo& info) noexcept {
        if (msg.is<fix::ExecutionReport>()) {
            const auto er = msg.as<fix::ExecutionReport>();
            (void)er.exec_id();
        }
        (void)info; // 崩溃恢复重投时 info.is_recovery == true
        return nf::Status::success();
    }
};

auto init = nf::DirectInitiator<fix::Schema, MyHandler>::open({
    .host = "127.0.0.1",
    .port = 9876,
    .storage_directory = "./state/buy",
    .local_comp_id = "BUY",
    .remote_comp_id = "SELL",
}).value();

(void)init.connect();
while (!init.is_active()) (void)init.poll_once();

const bool is_limit = true;
const std::optional<nf::Decimal> opt_px = nf::Decimal{15025, 2};
auto write_order = [&](auto& w) {
    // 可选：每笔可动态变化的 Standard Header 字段（须在正文前写入）：
    // w.header().sender_sub_id("DESK-A").target_sub_id("EXCH-1");
    w.cl_ord_id("ORD-001")
     .symbol("AAPL")
     .side('1')
     .transact_time("20261125-09:30:00.000")
     .order_qty(nf::Decimal{100, 0})
     .ord_type(is_limit ? '2' : '1')
     .price(opt_px); // std::nullopt 时自动跳过
};

// DirectInitiator 的三种可组合发送模式：
(void)init.send_stream<fix::NewOrderSingle>(write_order);       // 1) 单线程直发（单笔延迟最低）
// (void)init.enqueue_stream<fix::NewOrderSingle>(write_order); // 2) 同线程攒批 -> 1 次 sendmsg()
// (void)init.flush();
// nf::DirectSendQueue q{256, 512};                             // 3) 双线程无锁 SPSC 流水线：
// (void)q.try_push_stream<fix::NewOrderSingle>(write_order);   //    策略线程入队，
// (void)init.drain(q);                                         //    I/O 线程批量出队落盘并 1 次 sendmsg()
```

---

## 性能速览

在 Release 模式（`-O3`、GCC 16.2.1、AMD Ryzen 7 7840HS、Linux 7.2）下，启用全量 912 字段 `FIX44.xml` 严格校验与真实分段 mmap 日志持久化测得。所有 NimbleFIX 热路径均为 **0 堆分配**。完整测试方法与复现步骤见 [性能基准](docs/benchmarks.zh-CN.md)。

| 基准项 | NimbleFIX (p50 / p99) | 吞吐 | 堆分配 |
| :--- | ---: | ---: | :---: |
| **流式编码**（`NewOrderSingle` + `NoPartyIDs[3]`） | **140 ns** / **151 ns** | 7.09M msgs/s | 0 |
| **解析 + 912 字段严格校验 + 字段读取** | **421 ns** / **431 ns** | 2.36M msgs/s | 0 |
| **纯引擎 Pre-TCP 往返**（`DirectInitiator` + mmap 日志） | **942 ns** / **1.44 µs** | 1.06M rt/s | 0 |
| **本机 TCP Ping-Pong RTT**（`DirectInitiator`） | **9.67 µs** / **13.28 µs** | 104k rt/s | 0 |
| **本机 TCP 32 笔密集连发**（`DirectSendQueue`，CO-Free 单向） | **12.86 µs** / **22.64 µs** | 1.43M ord/s | 0 |
| **4 分片 `ShardedEngine` Acceptor**（16 并发会话，单端口） | **31.19 µs** / **49.99 µs** | 471k ord/s | 0 |

在严格同口径对比 QuickFIX 1.16.0（同一 `FIX44.xml` 严格校验与磁盘持久化）中：编码快 **8.6×**，解析校验快 **6.6×**，TCP 往返延迟低 **2.3×**，32 笔密集连发延迟低 **15.7×**。

---

## 构建与接入

拉取源码及子模块：

```sh
git clone --recurse-submodules <repo-url>
```

### 使用 CMake 构建与测试

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DNIMBLEFIX_ENABLE_TLS=ON -DNIMBLEFIX_ENABLE_XML=ON
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure
```

### 使用 xmake 构建与测试

```sh
xmake f -m release --tls=y --xml=y -y
xmake check
```

*（如需启用 Linux `io_uring` 后端，额外传入 `-DNIMBLEFIX_ENABLE_IO_URING=ON` 或 `--io_uring=y`。）*

### 在你的 CMake 项目中接入

```cmake
find_package(NimbleFIX CONFIG REQUIRED COMPONENTS core)

# 按需选择链接目标：
target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core)
# target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core_tls)
# target_link_libraries(my_gateway PRIVATE NimbleFIX::nimblefix)

# 可选：将自定义 FIX XML 字典编译为强类型 C++20 头文件：
nimblefix_generate_schema(TARGET my_venue_schema NAMESPACE my::venue
    HEADER venue.hpp XML_FILES "${CMAKE_CURRENT_SOURCE_DIR}/venue.xml")
target_link_libraries(my_trader PRIVATE my_venue_schema)
```

第三方依赖与许可信息见 [third_party/README.md](third_party/README.md)。
