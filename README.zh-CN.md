# NimbleFIX 0.3

[English](README.md) | **简体中文**

一个零外部脚本依赖、按**用多少付多少（Pay Only For What You Need）**设计的现代 C++20 FIX 协议引擎：可以从零开销的 Layer 0 **`DirectInitiator`** 起步（`NimbleFIX::nimblefix_core` —— 稳态 0 `malloc` / 0 `mutex` / 0 `fsync`，自带完整重启恢复与 GapFill/Replay），按需叠加 Layer 0 OpenSSL 3 TLS（`NimbleFIX::nimblefix_core_tls`），或直接扩展至 Layer 1/2 多会话 **`Engine`** 与多核 **`ShardedEngine`**（`NimbleFIX::nimblefix`），支持可插拔 **`IoBackend`**（`epoll` / `busy_poll` / 可选 Linux `io_uring`）、Acceptor、`Inline`/`Queued` 派发、MPSC/SPSC 跨线程发送、旁路 mmap 遥测与确定性历史回放。

- **纯 C++ 库 —— 零配置文件格式绑定**：库本身不内置、也不偏好任何配置文件格式，仅对外提供强类型 C++ 配置结构体（`DirectInitiatorConfig`、`EngineConfig`、`ShardedEngineConfig`、`TlsConfig`、`ConnectionStrategy` 等）。应用层可自由选择 JSON、YAML、TOML、数据库、命令行参数或环境变量来组装配置；可选独立命令行应用 `nimblefix`（`tools/nimblefix`）中的 TOML 解析仅作为应用层示例存在。
- **100% 纯 C++20 工具链 —— 零 Python 依赖**：字典代码生成器（`nimblefix-schema-gen`）、全部集成/契约/安装验证测试及基准测试验证器均已实现为原生 C++20 程序。构建、测试与安装 NimbleFIX **完全不需要 Python**。

[使用手册 (中文)](docs/user-guide.zh-CN.md) · [User Guide (EN)](docs/user-guide.md) · [架构设计](docs/architecture.md) · [支持边界矩阵](docs/session-support.md) · [性能基准](docs/benchmarks.md) · [路线图](docs/roadmap.md)

---

## 三层模块化架构

| 层级 | CMake 目标 / xmake 目标 | 包含能力 | 额外依赖 |
| :--- | :--- | :--- | :--- |
| **Layer 0（极速核心）** | `NimbleFIX::nimblefix_core` / `nimblefix-core` | 零中间拷贝**流式编码器 (`StreamWriter`)**、`Fixed<...>`、编译期生成字典（`nimblefix-schema-gen`）、分段 mmap Journal（SSE4.2 硬件 CRC32C、`reserve()`/`commit()`）、`SessionCore`、非阻塞 `TcpTransport`（`write_batch`/`sendmsg`）、**`DirectInitiator`**、多节点主备切换 **`ConnectionStrategy`**、无锁 SPSC **`DirectSendQueue`**、旁路 mmap 遥测 **`TelemetryWriter`/`TelemetryReader`**、历史回放 **`JournalReplayer`** | 仅 C++20 + `pthread` |
| **Layer 0 + TLS** | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | 包含全部 Layer 0 能力 + 非阻塞 OpenSSL 3 **`DirectTlsTransport`**（TLS 1.2/1.3、CA/SAN 校验、双向 mTLS、重连复用 `TlsContext`） | OpenSSL 3+（可选） |
| **Layer 1 / 2（完整运行时）** | `NimbleFIX::nimblefix` / `nimblefix` | 包含以上全部能力 + `SessionDriver`、可插拔 `IoBackend`（`epoll` / `busy_poll` / 可选 `io_uring`）、多会话 **`Engine<Schema, Inline\|Queued>`**、多核 **`ShardedEngine`**（单端口 Logon 路由移交、CPU 绑核、`IdleStrategy`）、Acceptor、MPSC `Sender`、SPSC `ProducerLane`、可选运行期 libxml2 XML（`NimbleFIX::nimblefix_xml`） | 可选 `liburing`、`libxml2` |

*（可选独立应用程序：`tools/nimblefix` 提供的 `nimblefix` CLI —— 不被库目标链接。）*

---

## 快速上手：Layer 0 `DirectInitiator`

完整可运行代码见 [examples/direct_initiator.cpp](examples/direct_initiator.cpp)，各场景推荐写法、详细线程模型与职责边界请阅读 [使用手册 (docs/user-guide.zh-CN.md)](docs/user-guide.zh-CN.md)。

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
        (void)info; // info.is_recovery == true 表示崩溃重启后的恢复重投
        return nf::Status::success();
    }
};

// 纯 C++ 配置结构体 —— 由你的应用程序从任意配置源填充
auto init = nf::DirectInitiator<fix::Schema, MyHandler>::open({
    .host = "127.0.0.1",
    .port = 9876,
    .storage_directory = "./state/buy",
    .local_comp_id = "BUY",
    .remote_comp_id = "SELL",
}).value();
(void)init.connect();
while (!init.is_active()) (void)init.poll_once();

// 零中间拷贝流式直接写入 mmap Journal（0 堆分配）：
const bool is_limit = true;
const std::optional<nf::Decimal> opt_px = nf::Decimal{15025, 2};
auto write_order = [&](auto& w) {
    // 可选：每笔可动态变化的 Standard Header 字段（必须在正文前写入）：
    // w.header().sender_sub_id("DESK-A").target_sub_id("EXCH-1");
    w.cl_ord_id("ORD-001").symbol("AAPL").side('1')
     .transact_time("20261123-00:00:00.000")
     .order_qty(nf::Decimal{100, 0})
     .ord_type(is_limit ? '2' : '1')
     .price(opt_px); // std::nullopt 自动跳过
    // 重复组（支持嵌套与运行期动态循环，0 堆分配）：
    // w.no_party_i_ds(1, [](auto& g) {
    //     g.entry().party_id("CLIENT1").party_id_source('D').party_role(1);
    // });
};

// DirectInitiator 的 3 种可组合发送模式：
(void)init.send_stream<fix::NewOrderSingle>(write_order);      // 1) 单线程直发（单笔延迟最低）
// (void)init.enqueue_stream<fix::NewOrderSingle>(write_order);// 2) 同线程攒批 -> 1 次 sendmsg/writev
// (void)init.flush();
// nf::DirectSendQueue q{256, 512};                            // 3) 双线程无锁 SPSC：
// (void)q.try_push_stream<fix::NewOrderSingle>(write_order);  //    策略线程流式写入无锁环槽，
// (void)init.drain(q);                                        //    I/O 线程批量补序列号落盘并 1 次 sendmsg
```

### 原生 C++20 字典编译器（`nimblefix-schema-gen`）

无需 Python，直接把标准或自定义 FIX XML 字典编译为零分配强类型 C++20 头文件：

```sh
# 完整字典：
./build-cmake/nimblefix-schema-gen --output generated/venue.hpp --namespace venue \
  schemas/standard/FIX44.xml

# 按业务消息子集裁剪（自动保留管理消息与关联重复组/组件）：
./build-cmake/nimblefix-schema-gen --output generated/venue.hpp --namespace venue \
  --messages NewOrderSingle,OrderCancelRequest,ExecutionReport schemas/standard/FIX44.xml

# 或直接从真实 FIX 日志（.log / .fix / .nfxj）自动裁剪未使用的消息与可选字段：
./build-cmake/nimblefix-schema-gen --output generated/venue.hpp --namespace venue \
  --from-fix-log ./logs/venue.log schemas/standard/FIX44.xml
```

CMake（`nimblefix_generate_schema(...)`）与 xmake（`add_rules("nimblefix.schema")`）均自动调用已安装的 `nimblefix-schema-gen` 可执行文件，并具备完整增量构建跟踪。

---

## 性能实测摘要（Release, AMD Ryzen 7 7840HS, GCC 16.2.1, Linux 7.2）

完整测试方法、边界披露与复现命令见 [docs/benchmarks.md](docs/benchmarks.md)。所有热路径均开启真实**分段 mmap Journal + SSE4.2 硬件 CRC32C**持久化，并通过 `-Wl,--wrap=malloc` 审计确认 **0 堆分配**。

### 1. `DirectInitiator` 分层耗时拆解（全量 912 字段 FIX 4.4 + `NoPartyIDs[3]` + mmap Journal）

| 测量边界 | p50 | p95 | p99 | 吞吐 | 堆分配 |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **纯流式编码 (`StreamWriter`)** (`encode_fix44_stream`) | **140.0 ns** | 150.0 ns | **151.0 ns** | **7,089,686 msgs/s** | **0** |
| **Pre-TCP 会话出站** (`DirectInitiator::send_stream` + mmap Journal) | **211.0 ns** | 280.0 ns | **421.0 ns** | **4,420,157 msgs/s** | **0** |
| **Pre-TCP 会话入站**（分帧 + 解析 + 912 字段校验 + 双 Journal 记录 + 回调） | **721.0 ns** | 822.0 ns | **1,062.0 ns** | **1,392,046 msgs/s** | **0** |
| **Pre-TCP 往返总计**（纯 FIX 引擎往返总预算） | **942.0 ns** | 1,091.0 ns | **1,442.0 ns** | **1,058,645 rt/s** | **0** |
| **真实 Loopback TCP RTT**（内核 TCP + 全量 FIX 4.4 + mmap Journal） | **9.67 µs** | 10.67 µs | 13.28 µs | 103,837 rt/s | **0** |

### 2. Initiator 发单模式对比（真实 Loopback TCP + FIX 4.4 + mmap Journal）

| 发单模式 | Ping-Pong 单向 p50 (RTT p50) | 纯密集连发 32×0 µs CO-Free p50 / p99 (吞吐) | 策略计算+连发 32×2 µs CO-Free p50 / p99 (吞吐) |
| :--- | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`**（单线程无队列） | **4.69 µs** (**8.86 µs**) | 49.93 µs / 98.19 µs (335k ord/s) | 86.87 µs / 228.77 µs (179k ord/s) |
| **2. `DirectInitiator::enqueue_stream()+flush()`**（单线程 `sendmsg`） | 5.42 µs (10.39 µs) | 14.58 µs / **19.83 µs** (1.27M ord/s) | 80.85 µs / 217.01 µs (320k ord/s) |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`**（双线程 SPSC） | 5.27 µs (9.09 µs) | **12.86 µs** / 22.64 µs (**1.43M ord/s**) | **42.62 µs** / **76.08 µs** (**422k ord/s**) |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`**（双线程 SPSC） | 7.69 µs (12.27 µs) | 67.28 µs / 117.28 µs (278k ord/s) | 64.77 µs / 118.97 µs (280k ord/s) |

### 3. 多核分片 Reactor 扩展性（单监听端口，全量 912 字段 FIX 4.4 + mmap Journal）

| 配置 | 并发会话数 | p50 | p99 | p99.9 | 总吞吐 | 相对单核加速 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **单核 `Engine` (`epoll`)** | 8 | 32.99 µs | 76.91 µs | 227.25 µs | 176,387 ord/s | 1.00× |
| **4 分片 `ShardedEngine` (`hybrid_backoff`，绑核)** | 8 | **16.39 µs** | **21.27 µs** | **28.98 µs** | **443,302 ord/s** | **2.51×**（p99.9 降低 **7.84×**） |
| **单核 `Engine` (`epoll`)** | 16 | 75.64 µs | 161.11 µs | 346.42 µs | 176,184 ord/s | 1.00× |
| **4 分片 `ShardedEngine` (`hybrid_backoff`，绑核)** | 16 | **31.19 µs** | **49.99 µs** | **96.74 µs** | **470,973 ord/s** | **2.67×**（p99 降低 **3.22×**） |

### 4. NimbleFIX 0.3 vs QuickFIX 1.16.0（严格同口径对比 `quickfix-parity-bench`）

| 测试项（1000 预热 / 5000 样本） | NimbleFIX 0.3 (p50 / p99 / 堆分配) | QuickFIX 1.16.0 (p50 / p99 / 堆分配) | 加速比 (p50) |
| :--- | :--- | :--- | ---: |
| **Codec 编码** (`StreamWriter` / `Fixed`) | **170 ns** / **371 ns** (**0.00** alloc) | 1,463 ns / 1,523 ns / 19.00 | **8.61×** / **3.94×** |
| **解析 + 全量 `FIX44.xml` 严格校验 + 读字段** | **421 ns** / **431 ns** / **0.00** | 2,795 ns / 3,386 ns / 15.00 | **6.64×** |
| **TCP Initiator RTT** (`DirectInitiator` / `Engine busy_poll`) | **10.21 µs** / **14.39 µs** (**0.00** alloc) | 23.72 µs / p99 34.56 µs / 35.06 | **2.32×** / 1.65× |
| **TCP Acceptor RTT** (`Engine busy_poll` / `epoll`) | **10.59 µs** / **11.57 µs** (**0.00** alloc) | 23.20 µs / p99 33.92 µs / 37.00 | **2.19×** / 2.01× |
| **TCP 32 笔密集连发** (`enqueue_stream` + `sendmsg`, CO-Free 单向) | **12.63 µs** / **23.51 µs** / **0.00** (**1.34M/s**) | 198.47 µs / 399.32 µs / 28.06 (80k/s) | **15.71×**（吞吐 **16.6×**） |

---

## 构建、测试与安装（零 Python）

拉取仓库及子模块（`third_party/tomlplusplus` 供可选 CLI 使用，`third_party/quickfix` 供可选互操作测试与性能对比使用）：

```sh
git clone --recurse-submodules <repo-url>
# 或在已有仓库中初始化子模块：
git submodule update --init --recursive
```

### xmake

```sh
xmake f -m release --tls=y --xml=y --benchmarks=y --quickfix=y -y
xmake check

# 可选开启 Linux io_uring Proactor 后端：
xmake f -m release --tls=y --xml=y --io_uring=y -y
xmake build acceptor-example initiator-example direct-initiator-example nimblefix-cli
```

### CMake

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DNIMBLEFIX_ENABLE_TLS=ON -DNIMBLEFIX_ENABLE_XML=ON \
  -DNIMBLEFIX_BUILD_BENCHMARKS=ON -DNIMBLEFIX_ENABLE_QUICKFIX=ON
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure

# 可重定位安装验证（纯 C++ 程序，零 Python）：
./build-cmake/install-test --root . --features --examples \
  --endpoint-test ./build-cmake/endpoint-test \
  --xmake-schema-test ./build-cmake/xmake_schema-test
./build-cmake/install-test --root . --core-only --examples \
  --endpoint-test ./build-cmake/endpoint-test \
  --xmake-schema-test ./build-cmake/xmake_schema-test
```

### 外部 CMake 项目接入

```cmake
find_package(NimbleFIX 0.3 CONFIG REQUIRED COMPONENTS core)

# 仅需 Layer 0 极速 Initiator（C++20 + pthread）：
target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core)
# Layer 0 + OpenSSL 3 TLS：
# target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core_tls)
# Layer 1/2 完整 Engine、ShardedEngine、Acceptor：
# target_link_libraries(my_gateway PRIVATE NimbleFIX::nimblefix)

# 可选：编译期 XML -> C++20 字典生成（自动调用 NimbleFIX::nimblefix-schema-gen）：
nimblefix_generate_schema(TARGET my_venue_schema NAMESPACE my::venue
    HEADER venue.hpp XML_FILES "${CMAKE_CURRENT_SOURCE_DIR}/venue.xml")
target_link_libraries(my_trader PRIVATE my_venue_schema)
```

第三方依赖与许可说明见 [third_party/README.md](third_party/README.md)。
