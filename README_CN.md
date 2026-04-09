# FastFix

**面向高频交易系统的低延迟 FIX 协议引擎。**

[English](README.md)

---

## 项目简介

FastFix 是一个用 C++20 从头实现的 FIX（Financial Information eXchange）协议引擎，专为延迟敏感的交易基础设施设计。它完整处理 FIX 会话生命周期——连接管理、Logon/Logout 握手、序列号追踪、心跳监控、gap 检测、重传恢复——同时保持热路径零分配、无锁。

FastFix 使用同一套内部引擎同时支持 **initiator**（客户端）和 **acceptor**（服务端），支持 FIX 4.2、4.3、4.4 和 FIXT.1.1。

## 为什么做这个项目？

现有的开源 FIX 引擎（QuickFIX、QuickFIX/J、Fix8）以正确性和广泛兼容性为设计目标，而非极致速度。它们把消息解析到动态 map 中，每条消息都做堆分配，用锁保护跨线程共享状态。这对 99% 的场景够用——但对微秒级延迟有刚需的团队来说不够。

FastFix 的设计出发点是：*如果每个设计决策都为热路径优化，会怎样？*

- **字典支撑编解码**：FIX 元数据会在启动时归一化到内存中，来源既可以是 `.ffd` 文本字典，也可以是预编译的 `.art` artifact。两条路径最终都进入同一套运行时字典表示。
- **零拷贝消息视图**：解析后的消息是对原始字节缓冲区的轻量视图，不拷贝，除非你显式请求。
- **单写者会话模型**：每个 session 由且仅由一个 worker 线程持有。关键路径上无锁、无竞争、无伪共享。
- **预编译帧模板**：高频消息类型的 header/trailer 片段预先构建，每条消息只需填充可变字段、BodyLength 和 Checksum。
- **SIMD 加速解析**：tokenizer 热循环中使用 SSE2 向量化扫描 SOH 分隔符和 `=` 分隔符。

## 亮点

| 特性 | 说明 |
|------|------|
| **编解码延迟** | FIX44 benchmark：header peek 160 ns (p50)，完整解析 912 ns (p50)，typed writer 编码 350 ns (p50) |
| **稳态零分配** | 解析路径零堆分配；编码路径仅 5 alloc/op |
| **完整会话管理** | Logon/Logout/Heartbeat/TestRequest/ResendRequest/SequenceReset |
| **嵌套重复组** | 通过字典元数据完整支持嵌套 repeating group |
| **自动重连退避** | initiator 可配置指数退避 + 随机抖动的自动重连 |
| **动态会话工厂** | acceptor 可通过回调或白名单接纳未知 CompID |
| **可插拔持久化** | Memory、mmap、durable batch store，支持可配置 rollover |
| **Worker 分片** | 每 worker 独立事件循环，支持 CPU 亲和性绑定 |
| **Busy-poll 模式** | 可选零超时轮询，实现最低延迟 |
| **SIMD tokenizer** | SSE2 字节扫描，自动回退到标量路径 |
| **可观测性** | 内置 metrics 注册表和环形缓冲 trace 记录器 |
| **压力测试** | 故障注入工具（gap、重复、乱序、断线重连） |
| **Fuzz 测试** | libFuzzer 工具覆盖 codec、config、dictionary 输入 |

## 与 QuickFIX 的差异

[QuickFIX](http://www.quickfixengine.org/) 是事实上的开源 FIX 引擎标准。它成熟、广泛使用、正确。FastFix 在设计理念上有根本不同：

| 方面 | QuickFIX | FastFix |
|------|----------|---------|
| **解析** | 运行时加载 XML 数据字典；字段存入 `std::map` | 启动时从 `.art` 或 `.ffd` 归一化到内存字典；热路径使用连续查找表 |
| **消息表示** | `FieldMap`，堆分配字符串 | 零拷贝 `MessageView`，直接引用原始字节 |
| **每消息分配** | 多次（map 插入、字符串拷贝） | 复用缓冲区编码路径为 0 次 |
| **线程模型** | 全局 session 锁，thread-per-session | 单写者无锁，分片 worker |
| **Group 处理** | 动态嵌套 + map 查找 | 字典驱动栈，预知结构 |
| **编码** | 收集字段 → 序列化 | 预编译帧模板，仅填充可变字段 |
| **语言标准** | C++（C++11 之前的风格） | C++20 |

---

## 快速开始

### 前置依赖

- C++20 编译器（GCC 12+、Clang 15+）
- CMake 3.20+ 和原生构建工具（`make` 或 Ninja）
- `xmake` 为可选项，保留给本地开发使用

### 离线依赖布局

FastFix 现在把必需的第三方源码改成固定版本的 Git submodule，便于离线构建：

- `deps/src/Catch2` — Catch2 v3.13.0
- `deps/src/pugixml` — pugixml v1.15
- `bench/vendor/quickfix` — QuickFIX 指定提交 `00dd20837c97578e725072e5514c8ffaa0e141d4`，并内置 `spec/FIX44.xml`

克隆后先执行一次：

```bash
git submodule update --init --recursive
```

完成 submodule 初始化后，构建过程中不再从网络下载 Catch2、pugixml、QuickFIX 或 FIX 字典。

### 构建

```bash
# 一键：配置、构建、测试并跑 benchmark smoke
bash ./scripts/offline_build.sh --preset dev-release --bench smoke

# 完整 benchmark compare
bash ./scripts/offline_build.sh --preset dev-release --bench full

# 离线目标 preset
cmake --preset rhel8-gcc12
cmake --build --preset rhel8-gcc12
ctest --preset rhel8-gcc12

cmake --preset rhel9-gcc14
cmake --build --preset rhel9-gcc14
ctest --preset rhel9-gcc14

# 手动 CMake 流程
cmake -S . -B build/cmake/dev-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake/dev-release
ctest --test-dir build/cmake/dev-release --output-on-failure

# 可选 xmake 流程：依赖已改为本地源码，不再触发在线包管理
xmake f -m release -y
xmake build fastfix-tests
```

GitHub Actions CI 也会在每次 push / pull request 中，通过 `ubi8/ubi:8.10` + `gcc-toolset-12` 和 `ubi9/ubi:9.7` + `gcc-toolset-14` 容器 job 实际跑这两个 RHEL preset。

如果 `build/generated/`、`build/bench/` 或 `build/sample-basic.art` 被删掉了，`xmake build fastfix-tests` 和 `xmake build fastfix-bench` 现在会自动把这些共享资产重新生成出来。

基于 preset 的 CMake 构建会把二进制写到 `build/cmake/<preset>/bin/`。无论使用哪套构建系统，测试和 benchmark 仍共用仓库根目录下的 `build/generated/` 与 `build/bench/` 资产路径。

### 运行测试

```bash
./build/cmake/dev-release/bin/fastfix-tests
# 或: bash ./scripts/offline_build.sh --preset dev-release --bench skip
# 或: ./build/linux/x86_64/release/fastfix-tests
```

### 集成方式

FastFix 编译为一个静态库 `libfastfix.a`。在你自己的程序中使用它：

1. **编译库**：`cmake --build build/cmake/dev-release --target fastfix` 或 `xmake build fastfix`
2. **编译协议 Profile**：运行 `fastfix-dictgen` 从字典文件生成 `.art` 二进制 artifact（可选，也可在运行时直接加载 `.ffd`）
3. **链接和引用头文件**：编译器指向 `include/` 目录，链接 `libfastfix.a`

**通过 xmake 链接（作为子依赖）：**

```lua
-- xmake.lua
includes("path/to/fastfix")   -- fastfix 源码目录的路径

target("my-trading-app")
    set_kind("binary")
    add_deps("fastfix")        -- 自动链接 libfastfix.a + 添加 include 路径
    add_files("src/*.cpp")
```

**通过 CMake / Makefile / 其他构建系统链接：**

```bash
# 1. 先编译 fastfix
cd path/to/fastfix && cmake -S . -B build/cmake/dev-release -DCMAKE_BUILD_TYPE=Release && cmake --build build/cmake/dev-release --target fastfix

# 2. 在你的构建系统中添加：
#    头文件路径:    path/to/fastfix/include
#    链接库:       path/to/fastfix/build/cmake/dev-release/lib/libfastfix.a
#    C++ 标准:     C++20
```

---

## 核心概念

### Profile

**Profile** 是一个 FIX 协议变体的编译后二进制描述——包含哪些字段、定义了哪些消息、消息里有什么 group。每个 profile 携带一个你自己指定的 `profile_id`（uint64）。Session 通过这个 ID 引用 profile，一个 Engine 可以同时加载多个 profile（如 FIX 4.2 和 FIX 4.4）。

### `.ffd` — FastFix 字典

文本文件，定义一个 FIX 变体的全部字段、消息和 group。其中包含你指定的 `profile_id`（任意唯一数字，例如 `1001` 代表 FIX 4.4）。可向 `fastfix-dictgen` 或 Engine 传入多个 `.ffd` 文件——第一个提供基线定义，其余文件作为增量覆盖层。

```
profile_id=1001

field|35|MsgType|string|0
field|11|ClOrdID|string|0
field|38|OrderQty|int|0
field|453|NoPartyIDs|int|0
field|448|PartyID|string|0

message|D|NewOrderSingle|0|35:r,11:r,38:r,453:o
group|453|448|Parties|0|448:r,447:r,452:r
```

格式：行级、管道符分隔。`#` 开头为注释。头部行为 `key=value`。字段定义：`field|tag|name|type|flags`。消息定义：`message|msg_type|name|admin_flag|field_rules`。Group 定义：`group|count_tag|delimiter_tag|name|flags|field_rules`。字段规则用 `tag:r`（必需）或 `tag:o`（可选）。

### `.art` — Artifact

`fastfix-dictgen` 的编译输出。扁平的、可 mmap 加载的二进制文件，包含字符串表、字段/消息/group 定义、校验规则和查找表。`.ffd` 中的 `profile_id` 会嵌入其中。加载 `.art` 避免启动时的文本解析——适用于生产部署或启动延迟敏感的场景。

### Overlay（覆盖层）

多个 `.ffd` 文件可以合并，用场所特定的自定义字段扩展基线 profile。第一个 `.ffd` 提供基线，后续文件添加字段、扩展消息或覆盖字段规则：

```
# venue_extensions.ffd — 自定义字段覆盖层
field|5001|VenueOrderType|string|1
field|5002|VenueAccount|string|1
message|D|NewOrderSingle|0|5001:r,5002:o
```

### MessageBuilder 与 FixedLayoutWriter

FastFix 提供两种方式构建出站消息：

- **`MessageBuilder`** — 通用灵活的 API。适用于任何消息类型，无需前置配置。适合低频消息或原型开发。
- **`FixedLayoutWriter`** — 热路径优化。需要从字典预先构建 `FixedLayout`，预计算 tag → slot 映射实现 O(1) 写入。高频消息类型显著更快。

### MessageView

零拷贝只读访问器，引用已解析 FIX 消息的原始字节缓冲区。无拷贝、无分配。访问器返回 `std::optional`（字段不存在则为空）。

### ValidationPolicy（校验策略）

控制解码时编解码器的严格程度：

| 模式 | 行为 |
|------|------|
| `kStrict` | 拒绝未知 tag，强制字段顺序，禁止重复 |
| `kCompatible` | 允许未知 tag，稍稍放宽顺序 |
| `kPermissive` | 允许未知 tag、重复、顺序违规 |
| `kRawPassThrough` | 接受任何合法的 FIX 字节流 |

---

## 工具

| 工具 | 用途 |
|------|------|
| `fastfix-dictgen` | 将 `.ffd` 字典编译为二进制 `.art` profile（支持合并多个 `.ffd` 文件，可生成 C++ builder 头文件） |
| `fastfix-xml2ffd` | 将 QuickFIX XML 数据字典转换为 `.ffd` 格式；也可从 `.ffd` 生成 C++ builder 头文件 |
| `fastfix-initiator` | CLI FIX 客户端，用于测试和互操作 |
| `fastfix-acceptor` | CLI FIX 服务端（回显服务器） |
| `fastfix-soak` | 带故障注入的压力测试（gap、重复、乱序、断线） |
| `fastfix-bench` | 延迟/吞吐量测试，含分配和 CPU 计数器追踪 |
| `fastfix-fuzz-codec` | libFuzzer，覆盖 codec 和 admin 协议 |
| `fastfix-fuzz-config` | libFuzzer，覆盖 `.ffcfg` 配置解析 |
| `fastfix-fuzz-dictgen` | libFuzzer，覆盖 `.ffd` 字典解析 |
| `fastfix-interop-runner` | 双向互操作场景运行器 |

### 编译 Profile

```bash
./build/linux/x86_64/release/fastfix-dictgen     --input samples/basic_profile.ffd     --merge samples/basic_overlay.ffd     --output build/sample-basic.art     --cpp-builders build/generated/sample_basic_builders.h
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `--input` | 是 | 基线字典文件（`.ffd`） |
| `--merge` | 否 | 额外 `.ffd` 文件（可重复指定） |
| `--output` | 是 | 输出 artifact 路径（`.art`） |
| `--cpp-builders` | 否 | 生成包含 profile 常量的 C++ 头文件 |

### 从 QuickFIX XML 转换

```bash
./build/linux/x86_64/release/fastfix-xml2ffd     --xml FIX44.xml     --output my_profile.ffd     --profile-id 1001     --cpp-builders generated_builders.h
```

Component 会被自动内联展开，group 会被自动提取，XML 类型会自动映射到 FastFix 类型，`schema_hash` 会自动计算。

---

## 用法：Initiator

### 标准 Initiator

```cpp
#include "fastfix/runtime/engine.h"
#include "fastfix/runtime/live_initiator.h"
#include "fastfix/runtime/config.h"
#include "fastfix/runtime/application.h"

class MyApp : public fastfix::runtime::ApplicationCallbacks {
    auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event)
        -> fastfix::base::Status override {
        if (event.session_event == fastfix::runtime::SessionEventKind::kActive) {
            // 会话已激活——开始发单
            auto msg = fastfix::message::MessageBuilder{"D"}
                .set_string(11, "ORD-001")
                .set_char(54, '1')       // Side=Buy
                .set_float(44, 150.25)   // Price
                .set_int(38, 100)        // OrderQty
                .build();
            event.handle.Send(std::move(msg));
        }
        return fastfix::base::Status::Ok();
    }

    auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event)
        -> fastfix::base::Status override {
        auto view = event.message_view();
        auto msg_type = view.msg_type();      // "8" = ExecutionReport
        auto exec_id = view.get_string(17);   // ExecID
        return fastfix::base::Status::Ok();
    }
};

int main() {
    fastfix::runtime::EngineConfig config;
    config.worker_count = 1;
    config.profile_artifacts = {"build/sample-basic.art"};
    // 或直接加载 .ffd:
    // config.profile_dictionaries = {{"samples/basic_profile.ffd"}};
    config.counterparties = {{
        .name = "venue-a",
        .session = {
            .session_id = 1,
            .key = {"FIX.4.4", "MY_FIRM", "VENUE_A"},
            .profile_id = 1001,
            .heartbeat_interval_seconds = 30,
            .is_initiator = true,
        },
        .reconnect_enabled = true,
        .reconnect_initial_ms = 1000,
        .reconnect_max_ms = 30000,
    }};

    fastfix::runtime::Engine engine;
    engine.LoadProfiles(config);
    engine.Boot(config);

    auto app = std::make_shared<MyApp>();
    fastfix::runtime::LiveInitiator initiator(&engine, {
        .application = app,
    });

    initiator.OpenSession(1, "exchange.example.com", 9876);
    initiator.Run();
}
```

### 热路径 Initiator：使用 FixedLayoutWriter

对于延迟敏感的下单场景，使用 `FixedLayoutWriter` 配合预构建的 `FixedLayout`：

```cpp
#include "fastfix/message/fixed_layout_writer.h"
#include "fastfix/codec/fix_codec.h"

// 启动时构建一次：
auto layout = fastfix::message::FixedLayout::Build(dictionary, "D").value();
auto templates = fastfix::codec::PrecompiledTemplateTable{};

// ... 在 OnSessionEvent 中，当 session 变为 active 时：

class HotPathApp : public fastfix::runtime::ApplicationCallbacks {
    fastfix::message::FixedLayout layout_;
    fastfix::codec::EncodeBuffer encode_buffer_;   // 跨调用复用
    fastfix::codec::EncodeOptions encode_options_;

    auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event)
        -> fastfix::base::Status override {
        if (event.session_event == fastfix::runtime::SessionEventKind::kActive) {
            SendOrder(event.handle);
        }
        return fastfix::base::Status::Ok();
    }

    void SendOrder(fastfix::session::SessionHandle handle) {
        fastfix::message::FixedLayoutWriter writer(layout_);
        writer.set_string(11, "ORD-001");    // ClOrdID
        writer.set_char(54, '1');             // Side=Buy
        writer.set_int(38, 100);             // OrderQty
        writer.set_float(44, 150.25);        // Price
        writer.set_string(55, "AAPL");       // Symbol

        // 添加 repeating group
        writer.add_group_entry(453)
            .set_string(448, "PARTY-A")
            .set_char(447, 'D')
            .set_int(452, 3);

        // 直接编码到可复用缓冲区——零分配
        auto status = writer.encode_to_buffer(dictionary_, encode_options_, &encode_buffer_);
        // ... 通过 handle 发送编码后的字节
    }
};
```

`FixedLayoutWriter` 路径的 benchmark 结果为 **~684 ns**（p50 = 651 ns），比相同消息结构下的通用 `MessageBuilder` 路径快约 **46%**。

### 混合路径——FixedLayout + 扩展字段

当大部分字段在编译时已知、但部分字段是动态的（如场所自定义扩展）：

```cpp
fastfix::message::FixedLayoutWriter writer(layout_);
// 已知字段通过 O(1) slot 写入
writer.set_string(11, "ORD-001");
writer.set_char(54, '1');
writer.set_int(38, 100);

// 不在 layout 中的动态/扩展字段
writer.set_extra_string(5001, "LIMIT");      // VenueOrderType
writer.set_extra_string(5002, "ACCT-XYZ");   // VenueAccount

auto status = writer.encode_to_buffer(dictionary, options, &buffer);
```

---

## 用法：Acceptor

### 使用动态 Session 工厂的标准 Acceptor

```cpp
#include "fastfix/runtime/engine.h"
#include "fastfix/runtime/live_acceptor.h"
#include "fastfix/runtime/config.h"

fastfix::runtime::EngineConfig config;
config.worker_count = 2;
config.profile_artifacts = {"build/sample-basic.art"};
config.listeners = {{
    .name = "main",
    .host = "0.0.0.0",
    .port = 9876,
}};
config.accept_unknown_sessions = true;

fastfix::runtime::Engine engine;
engine.LoadProfiles(config);
engine.Boot(config);

// 根据入站 Logon 动态接受 session：
engine.SetSessionFactory([](const fastfix::session::SessionKey& key)
    -> fastfix::base::Result<fastfix::runtime::CounterpartyConfig> {
    return fastfix::runtime::CounterpartyConfig{
        .name = key.sender_comp_id,
        .session = {
            .profile_id = 1001,
            .key = key,
            .heartbeat_interval_seconds = 30,
        },
    };
});

auto app = std::make_shared<MyApp>();
fastfix::runtime::LiveAcceptor acceptor(&engine, {
    .application = app,
});
acceptor.OpenListeners("main");
acceptor.Run();
```

### 使用预配置对端的 Acceptor

```cpp
fastfix::runtime::EngineConfig config;
config.worker_count = 1;
config.profile_artifacts = {"build/sample-basic.art"};
config.listeners = {{
    .name = "main",
    .host = "0.0.0.0",
    .port = 9876,
}};
config.counterparties = {{
    .name = "client-a",
    .session = {
        .session_id = 1,
        .key = {"FIX.4.4", "SERVER", "CLIENT_A"},
        .profile_id = 1001,
        .heartbeat_interval_seconds = 30,
        .is_initiator = false,
    },
    .store_mode = fastfix::runtime::StoreMode::kDurableBatch,
    .store_path = "/var/lib/fastfix/client-a",
}};

fastfix::runtime::Engine engine;
engine.LoadProfiles(config);
engine.Boot(config);

auto app = std::make_shared<MyApp>();
fastfix::runtime::LiveAcceptor acceptor(&engine, {
    .application = app,
});
acceptor.OpenListeners("main");
acceptor.Run();
```

### 热路径 Acceptor：读取入站消息

在 `OnAppMessage` 回调中，使用零拷贝 `MessageView` 以最小开销读取消息：

```cpp
auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event)
    -> fastfix::base::Status override {
    auto view = event.message_view();

    // 零拷贝字段访问——无分配
    auto cl_ord_id = view.get_string(11);   // → optional<string_view>
    auto side = view.get_char(54);           // → optional<char>
    auto qty = view.get_int(38);            // → optional<int64_t>
    auto price = view.get_float(44);        // → optional<double>

    // Repeating group 访问
    if (auto group = view.group(453)) {
        for (std::size_t i = 0; i < group->size(); ++i) {
            auto entry = group->entry(i);
            auto party_id = entry.get_string(448);
        }
    }

    // 构建并发送响应
    auto response = fastfix::message::MessageBuilder{"8"}
        .set_string(17, "EXEC-001")        // ExecID
        .set_string(11, cl_ord_id.value_or(""))
        .set_char(150, '0')                // ExecType=New
        .set_int(14, 0)                    // CumQty
        .build();
    event.handle.Send(std::move(response));

    return fastfix::base::Status::Ok();
}
```

---

## 配置参考

### EngineConfig（引擎配置）

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `worker_count` | uint32 | 1 | Worker 线程数 |
| `poll_mode` | enum | `kBlocking` | `kBlocking`（epoll 带超时）或 `kBusy`（自旋轮询，最低延迟） |
| `enable_metrics` | bool | true | 启用内置指标收集 |
| `trace_mode` | enum | `kDisabled` | `kDisabled` 或 `kRing`（环形缓冲 trace） |
| `trace_capacity` | uint32 | 0 | 环形 trace 缓冲区大小 |
| `profile_artifacts` | paths | — | 编译好的 `.art` profile 文件路径 |
| `profile_dictionaries` | path[][] | — | 运行时直接加载的 `.ffd` 字典文件组（每个内层向量作为基线 + 覆盖层合并） |
| `profile_madvise` | bool | false | 对加载的 artifact 调用 `madvise(MADV_WILLNEED)` |
| `profile_mlock` | bool | false | 对加载的 artifact 调用 `mlock()`（需足够的 RLIMIT） |
| `front_door_cpu` | uint32? | — | 绑定 acceptor 前门线程到指定 CPU 核 |
| `worker_cpu_affinity` | uint32[] | — | 绑定 worker 线程到 CPU 核 |
| `queue_app_mode` | enum | `kCoScheduled` | `kCoScheduled`（在 worker 线程上 drain）或 `kThreaded`（每 worker 一个专用 app 线程） |
| `app_cpu_affinity` | uint32[] | — | 绑定 app 线程到 CPU 核（`kThreaded` 时） |
| `accept_unknown_sessions` | bool | false | 允许动态 session factory 接纳未知入站 CompID |
| `listeners` | list | — | TCP 侦听配置（仅 acceptor） |
| `counterparties` | list | — | 预配置的 session 对端 |

### ListenerConfig（侦听配置）

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `name` | string | — | 侦听器标识（用于 `OpenListeners()` 调用） |
| `host` | string | `"0.0.0.0"` | 绑定地址 |
| `port` | uint16 | 0 | 侦听端口 |
| `worker_hint` | uint32 | 0 | Worker 池路由提示 |

### CounterpartyConfig（对端配置）

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `name` | string | — | 可读的对端名称 |
| `session.session_id` | uint64 | — | 唯一会话标识 |
| `session.key` | SessionKey | — | `{begin_string, sender_comp_id, target_comp_id}` |
| `session.profile_id` | uint64 | — | 使用的 profile ID |
| `session.heartbeat_interval_seconds` | uint32 | 30 | FIX 心跳间隔（秒） |
| `session.is_initiator` | bool | false | true = initiator，false = acceptor |
| `session.default_appl_ver_id` | string | — | FIXT.1.1 session 的默认 ApplVerID |
| `store_mode` | enum | `kMemory` | `kMemory`、`kMmap` 或 `kDurableBatch` |
| `store_path` | path | — | mmap/durable store 文件目录 |
| `recovery_mode` | enum | `kMemoryOnly` | `kMemoryOnly`、`kWarmRestart` 或 `kColdStart` |
| `dispatch_mode` | enum | `kInline` | `kInline`（在 worker 线程回调）或 `kQueueDecoupled`（SPSC 队列） |
| `validation_policy` | policy | `Strict()` | `Strict()`、`Compatible()`、`Permissive()` 或 `RawPassThrough()` |
| `reconnect_enabled` | bool | false | 启用 initiator 自动重连 |
| `reconnect_initial_ms` | uint32 | 1000 | 初始退避延迟（ms） |
| `reconnect_max_ms` | uint32 | 30000 | 最大退避延迟（ms） |
| `reconnect_max_retries` | uint32 | 0 | 最大重连次数（0 = 无限） |
| `durable_flush_threshold` | uint32 | 0 | Durable store 批量刷写阈值 |
| `durable_rollover_mode` | enum | `kUtcDay` | `kUtcDay`、`kLocalTime` 或 `kExternal` |
| `durable_archive_limit` | uint32 | 0 | 最大归档段数（0 = 不限） |
| `durable_local_utc_offset_seconds` | int32 | 0 | 本地时间 rollover 的 UTC 偏移 |
| `durable_use_system_timezone` | bool | true | 使用系统时区进行 rollover |

---

## 线程模型

FastFix 不会在你背后偷偷创建线程。每个线程都是显式配置、显式启动的，并可绑定到指定的 CPU 核心。

### 线程拓扑

**Acceptor（服务端）：**

| 线程 | 命名 | 数量 | 职责 | CPU 绑定 |
|------|------|------|------|---------|
| 前门 | `ff-acc-main` | 1 | accept TCP 连接 → 读 Logon → 路由到负载最低的 worker | `front_door_cpu` |
| 会话 worker | `ff-acc-w{N}` | `worker_count` | 独占 session：解码、序号、定时器、编码、发送 | `worker_cpu_affinity[N]` |
| 应用 worker | `ff-app-w{N}` | `worker_count` | 消费 SPSC 队列，调用业务回调 | `app_cpu_affinity[N]` |

**Initiator（客户端）：** 与 acceptor 相同但没有前门线程。会话 worker 命名为 `ff-ini-w{N}`。

`worker_count=1` 时，唯一的 worker 直接在调用者线程上运行——不会额外创建线程。应用 worker 仅在 `queue_app_mode = kThreaded` 时存在。

### Session 所有权

每个 session 严格归属一个 worker 线程。该 worker 独占处理所有协议状态：解码、序号管理、定时器、持久化、编码和发送。**热路径无锁。**

跨线程访问 session 通过 `SessionHandle`：

```cpp
// 从任意线程——线程安全：
session_handle.Send(msg);           // 入队消息 → 唤醒目标 worker
session_handle.Snapshot();          // 只读查询
session_handle.Subscribe();         // 事件订阅

// 非线程安全——仅限所属 worker / 回调作用域：
message_view.get_string(11);
```

### 应用回调模式

| 模式 | 你的代码在哪运行 | 约束 |
|------|-----------------|------|
| `kInline`（默认） | 在会话 worker 线程内，I/O 循环中 | 不能阻塞 |
| `kQueueDecoupled` + `kCoScheduled` | 在会话 worker 线程内，显式 poll 调用时 | 不能阻塞 |
| `kQueueDecoupled` + `kThreaded` | 在专用应用 worker 线程（`ff-app-wN`）上 | 可以阻塞（适度） |

**Inline 模式**延迟最低——`OnAppMessage()` 回调直接收到零拷贝的 `MessageView`，无额外开销。但回调必须快速返回，任何阻塞都会卡住该 worker 上的所有 session。

**Queue-decoupled / threaded 模式**增加一次 SPSC 队列传递，但完全隔离 I/O 和业务逻辑。队列溢出策略：`kCloseSession`、`kBackpressure`、`kDropNewest`。

### 典型低延迟部署

```
Core 0:  前门（仅 accept）
Core 1:  会话 worker 0  ← 热路径，独占核心
Core 2:  会话 worker 1
Core 3:  会话 worker 2
Core 4:  应用 worker 0  （kThreaded 时）
Core 5:  应用 worker 1
Core 6:  应用 worker 2
```

```cpp
config.worker_count = 3;
config.front_door_cpu = 0;
config.worker_cpu_affinity = {1, 2, 3};
config.app_cpu_affinity = {4, 5, 6};
config.poll_mode = fastfix::runtime::PollMode::kBusy;
config.queue_app_mode = fastfix::runtime::QueueAppThreadingMode::kThreaded;
```

---

## 性能测试

### 为什么和 QuickFIX 对比？

QuickFIX 是使用最广泛的开源 FIX 引擎，是最自然的基准线。和 QuickFIX 对比能清楚展示延迟差异的根源——解析策略、内存分配模式和线程模型。

### 测试方法

- **QuickFIX 来源**：GitHub 上的 `quickfix/quickfix` C++ 仓库，commit `00dd20837c97578e725072e5514c8ffaa0e141d4`
- **字典来源**：QuickFIX 的 `spec/FIX44.xml`；FastFix 侧使用同一个 XML，通过 `fastfix-xml2ffd` + `fastfix-dictgen` 转换
- **消息**：合法的 FIX.4.4 `NewOrderSingle`，包含 `ClOrdID`、`Symbol`、`Side`、`TransactTime`、`OrderQty`、`OrdType`、`Price`，以及一条 `NoPartyIDs=1` repeating group
- **分配统计**：通过全局 `operator new` 拦截统计每次堆分配
- **CPU 计数器**：使用 Linux `perf_event_open` 采集 cache miss 和 branch miss
- **分位数**：按每次迭代的 `steady_clock` 采样计算 p50、p95、p99、p999

### Benchmark 层级

| 层级 | 测量内容 |
|------|----------|
| **encode** | 业务对象 → 生成的 typed writer → 报文字节（复用缓冲区） |
| **peek** | 提取 session header（MsgType、CompID），不做完整解码 |
| **parse** | 完整消息解码为零拷贝 `MessageView` |
| **session-inbound** | 完整入站路径：解码 → 序列号校验 → 持久化 → admin 协议 |
| **replay** | ResendRequest 处理：取回 + 重新编码 128 条已存消息 |
| **loopback-roundtrip** | 完整 TCP 往返：发送 `NewOrderSingle`，等待返回 `ExecutionReport` |

### 测试结果

**测试环境：**

| | |
|---|---|
| **CPU** | AMD Ryzen 7 7840HS（8 核 16 线程，睿频 5.1 GHz） |
| **内存** | 28 GB DDR5 |
| **操作系统** | Linux 6.19.6 CachyOS（x86_64） |
| **编译器** | GCC 15.2.1 20260209 |
| **构建模式** | Release（`-O2`） |
| **迭代次数** | 编解码 100,000 次；replay/loopback 为 1,000 次 |

#### 可直接对比的编解码层级

| 边界 | FastFix 指标 | QuickFIX 指标 | FastFix p50 | FastFix p95 | QuickFIX p50 | QuickFIX p95 | FF 每次分配 | QF 每次分配 |
|------|--------------|---------------|-------------|-------------|--------------|--------------|-------------|-------------|
| encode（对象 → 报文） | `encode` | `quickfix-encode-buffer` | 0.35 µs | 0.37 µs | 1.35 µs | 1.47 µs | 5 | 29 |
| parse（报文 → 对象） | `parse` | `quickfix-parse` | 0.91 µs | 0.92 µs | 0.85 µs | 0.87 µs | 0 | 15 |

关键观察：
- **FastFix encode 比 QuickFIX 快约 4 倍**——生成的 typed writer 基于 `FixedLayoutWriter`，消除了动态派发和大部分分配。
- **QuickFIX 仍在纯 parse 路径上占优**——FastFix 的字典驱动解码每字段做的工作更多。
- **FastFix 的分配压力显著更低**：encode 为 5/op vs 29/op；parse 为 0/op vs 15/op。

#### FastFix 专有层级

| 层级 | p50 | p95 | p99 | 每次分配 | 吞吐量 |
|------|-----|-----|-----|----------|--------|
| peek | 160 ns | 170 ns | 180 ns | 0 | 5,460,000/s |
| session-inbound | 2.18 µs | 2.56 µs | 3.86 µs | 8 | 418,000/s |
| replay（128 msgs） | 249 µs | 254 µs | 256 µs | 0 | 4,100/s |
| loopback-roundtrip | 25.66 µs | 28.97 µs | 32.27 µs | 7 | 37,755/s |

### 自行运行测试

```bash
./bench/bench.sh build
./bench/bench.sh fastfix        # FastFix 主测试（artifact）
./bench/bench.sh fastfix-ffd    # FastFix 测试（直接加载 .ffd）
./bench/bench.sh quickfix       # QuickFIX 对比测试
./bench/bench.sh builder        # 仅 object-to-wire 对比
./bench/bench.sh compare        # 完整跨引擎对比
```

上面所有 benchmark 命令都明确使用固定的 QuickFIX 4.4 输入：`bench/vendor/quickfix/spec/FIX44.xml`、`build/bench/quickfix_FIX44.ffd` 或 `build/bench/quickfix_FIX44.art`。

完整的 benchmark 方法论、指标定义和详细结果表格，见 [bench/README.md](bench/README.md)。

---

## 更多文档

- [docs/architecture.md](docs/architecture.md) — 内部架构、模块依赖图、数据流图、线程模型细节
- [docs/development.md](docs/development.md) — 开发指南、测试、`.ffd` 格式规范、性能分析、压力/Fuzz 测试、引擎扩展
- [bench/README.md](bench/README.md) — Benchmark 基础设施、测量边界、完整结果表格

## 项目状态

FastFix 正在积极开发中。核心引擎、会话管理、编解码器和持久化层已实现并测试（152 test case，2619 assertion）。热路径编码管线持续向设计目标优化中。

## 许可证

详见 [LICENSE](LICENSE)。
