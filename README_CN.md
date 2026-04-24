# NimbleFIX

**面向延迟敏感交易系统的精干、低延迟 FIX 引擎。**

[English](README.md)

---

## 项目简介

NimbleFIX 是一个用 C++20 从头实现的 FIX（Financial Information eXchange）协议引擎，专为延迟敏感的交易基础设施设计。它完整处理 FIX 会话生命周期——连接管理、Logon/Logout 握手、序列号追踪、心跳监控、gap 检测、重传恢复——同时保持热路径零分配、无锁。

NimbleFIX 使用同一套内部引擎同时支持 **initiator**（客户端）和 **acceptor**（服务端），支持 FIX 4.2、4.3、4.4 和 FIXT.1.1。

## 为什么做这个项目？

现有的开源 FIX 引擎（QuickFIX、QuickFIX/J、Fix8）以正确性和广泛兼容性为设计目标，而非极致速度。它们把消息解析到动态 map 中，每条消息都做堆分配，用锁保护跨线程共享状态。这对 99% 的场景够用——但对微秒级延迟有刚需的团队来说不够。

NimbleFIX 的设计出发点是：*如果每个设计决策都为热路径优化，会怎样？*

- **字典支撑编解码**：FIX 元数据会在启动时归一化到内存中，来源既可以是 `.ffd` 文本字典，也可以是预编译的 `.art` artifact。两条路径最终都进入同一套运行时字典表示。
- **零拷贝消息视图**：解析后的消息是对原始字节缓冲区的轻量视图，不拷贝，除非你显式请求。
- **单写者会话模型**：每个 session 由且仅由一个 worker 线程持有。关键路径上无锁、无竞争、无伪共享。
- **预编译帧模板**：高频消息类型的 header/trailer 片段预先构建，每条消息只需填充可变字段、BodyLength 和 Checksum。
- **SIMD 加速解析**：tokenizer 热循环中使用 SSE2 向量化扫描 SOH 分隔符和 `=` 分隔符。

## 亮点

| 特性 | 说明 |
|------|------|
| **编解码延迟** | 当前 FIX44 对比实测：header peek 130 ns (p50)，完整解析 511 ns (p50)，typed writer 编码 371 ns (p50) |
| **稳态零分配** | 当前 compare 实测：encode/parse/session/replay 全部 0 alloc/op；loopback 为 3 alloc/op |
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

[QuickFIX](http://www.quickfixengine.org/) 是事实上的开源 FIX 引擎标准。它成熟、广泛使用、正确。NimbleFIX 在设计理念上有根本不同：

| 方面 | QuickFIX | NimbleFIX |
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
- `xmake` 3.0.0+（直连 xmake 路径时推荐）
- 或 CMake 3.20+ 加 Ninja（CMake 首选生成器），以及在没有 Ninja 时用 make 兜底

### 离线依赖布局

NimbleFIX 现在把必需的第三方源码改成固定版本的 Git submodule，便于离线构建：

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
# 自动探测路径：xmake -> cmake + Ninja -> cmake + make
bash ./scripts/offline_build.sh --bench smoke

# 用同样的自动探测顺序跑完整 benchmark compare
bash ./scripts/offline_build.sh --bench full

# 直接走 xmake
xmake f -m release --ccache=n -y
xmake build nimblefix-tests
xmake build nimblefix-bench

# 备用 CMake 路径
bash ./scripts/offline_build.sh --build-system cmake --preset dev-release --bench smoke

# 显式强制 make 兜底
bash ./scripts/offline_build.sh --build-system cmake --cmake-generator make --preset dev-release --bench smoke

# 离线目标 preset（默认优先 Ninja）
cmake --preset rhel8-gcc12
cmake --build --preset rhel8-gcc12
ctest --preset rhel8-gcc12

cmake --preset rhel9-gcc14
cmake --build --preset rhel9-gcc14
ctest --preset rhel9-gcc14

# 手动 CMake 流程
cmake -S . -B build/cmake/dev-release-manual -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake/dev-release-manual
ctest --test-dir build/cmake/dev-release-manual --output-on-failure

# Ninja 不可用时的 make 兜底
cmake -S . -B build/cmake/dev-release-manual-make -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake/dev-release-manual-make
ctest --test-dir build/cmake/dev-release-manual-make --output-on-failure

# 直接用 xmake 编译目标：依赖已改为本地源码，不再触发在线包管理
xmake f -m release --ccache=n -y
xmake build nimblefix-tests
```

辅助脚本现在会按 `xmake >= 3.0.0`、`cmake + Ninja`、`cmake + make` 的顺序自动选择。Ubuntu 24.04 仓库里的 xmake 目前只有 `2.8.7`，因此 helper 会在那里明确打印并回退到 CMake，除非你额外安装更高版本的 xmake。两个 helper 在 xmake 路径下也默认 `NIMBLEFIX_XMAKE_CCACHE=n`，用于规避 Linux 上 `.build_cache/... file busy` 的已知问题。

GitHub Actions CI 现在就在 Ubuntu 上使用这套自动选择逻辑，同时继续通过 `ubi8/ubi:8.10` + `gcc-toolset-12` 和 `ubi9/ubi:9.7` + `gcc-toolset-14` 容器 job 实际跑这两个 RHEL CMake preset。

如果 `build/generated/`、`build/bench/` 或 `build/sample-basic.art` 被删掉了，`xmake build nimblefix-tests` 和 `xmake build nimblefix-bench` 现在会自动把这些共享资产重新生成出来。

辅助脚本会自动按 `xmake`、`cmake + Ninja`、`cmake + make` 的顺序选择。默认 xmake 构建会把二进制写到 `build/linux/x86_64/release/`。基于 Ninja 的 CMake preset 会写到 `build/cmake/<preset>/bin/`，基于 make 的兜底 preset 会写到 `build/cmake/<preset>-make/bin/`。无论使用哪套构建系统，测试和 benchmark 仍共用仓库根目录下的 `build/generated/` 与 `build/bench/` 资产路径。

### 运行测试

```bash
./build/linux/x86_64/release/nimblefix-tests
# 或：一键路径: bash ./scripts/offline_build.sh --bench skip
# 或：CMake 备用二进制: ./build/cmake/dev-release/bin/nimblefix-tests
# 或：make 兜底二进制: ./build/cmake/dev-release-make/bin/nimblefix-tests
```

### 集成方式

NimbleFIX 当前编译为一个静态库 `libnimblefix.a`。在你自己的程序中使用它：

1. **编译库**：`xmake f -m release -y && xmake build nimblefix`；如果需要备用路径，再用 `cmake --build build/cmake/dev-release --target nimblefix`，而强制 make 兜底时对应目录是 `build/cmake/dev-release-make`
2. **编译协议 Profile**：运行 `nimblefix-dictgen` 从字典文件生成 `.art` 二进制 artifact（可选，也可在运行时直接加载 `.ffd`）
3. **链接和引用头文件**：编译器指向 `include/public/` 目录，链接 `libnimblefix.a`

**通过 xmake 链接（作为子依赖）：**

```lua
-- xmake.lua
includes("path/to/nimblefix")   -- nimblefix 源码目录的路径

target("my-trading-app")
    set_kind("binary")
    add_deps("nimblefix")        -- 自动链接 libnimblefix.a + 添加 include/public/
    add_files("src/*.cpp")
```

**通过 CMake / Makefile / 其他构建系统链接（备用路径）：**

```bash
# 1. 先编译 nimblefix
cd path/to/nimblefix && cmake -S . -B build/cmake/dev-release -DCMAKE_BUILD_TYPE=Release && cmake --build build/cmake/dev-release --target nimblefix

# 2. 在你的构建系统中添加：
#    头文件路径:    path/to/nimblefix/include/public
#    链接库:       path/to/nimblefix/build/cmake/dev-release/lib/libnimblefix.a
#    C++ 标准:     C++20
```

### 公共头文件

外部使用方应当只直接包含 `include/public/nimblefix/` 导出的头；`include/internal/nimblefix/` 是仓库内部实现头，不属于对外支持的 include 面。

大多数应用直接 include 下面这些头就够了：

- `nimblefix/runtime/application.h`
- `nimblefix/runtime/config.h`
- `nimblefix/runtime/engine.h`
- `nimblefix/runtime/live_acceptor.h`
- `nimblefix/runtime/live_initiator.h`
- `nimblefix/message/message_builder.h`
- `nimblefix/message/message_view.h`
- `nimblefix/message/fixed_layout_writer.h`
- `nimblefix/codec/fix_codec.h`
- `nimblefix/codec/fix_tags.h`
- `nimblefix/profile/profile_loader.h`
- `nimblefix/store/memory_store.h`
- `nimblefix/store/mmap_store.h`
- `nimblefix/store/durable_batch_store.h`
- `nimblefix/session/session_handle.h`

高级用法再按需补 `nimblefix/session/admin_protocol.h`、`nimblefix/session/resend_recovery.h`、`nimblefix/runtime/sharded_runtime.h`、`nimblefix/runtime/metrics.h`、`nimblefix/runtime/trace.h`。完整的导出头策略见 [docs/public-api.md](docs/public-api.md)。

---

## 核心概念

### Profile

**Profile** 是一个 FIX 协议变体的编译后二进制描述——包含哪些字段、定义了哪些消息、消息里有什么 group。每个 profile 携带一个你自己指定的 `profile_id`（uint64）。Session 通过这个 ID 引用 profile，一个 Engine 可以同时加载多个 profile（如 FIX 4.2 和 FIX 4.4）。

### `.ffd` — NimbleFIX 字典

文本文件，定义一个 FIX 变体的全部字段、消息和 group。其中包含你指定的 `profile_id`（任意唯一数字，例如 `1001` 代表 FIX 4.4）。可向 `nimblefix-dictgen` 或 Engine 传入多个 `.ffd` 文件——第一个提供基线定义，其余文件作为增量覆盖层。

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

`nimblefix-dictgen` 的编译输出。扁平的、可 mmap 加载的二进制文件，包含字符串表、字段/消息/group 定义、校验规则和查找表。`.ffd` 中的 `profile_id` 会嵌入其中。加载 `.art` 避免启动时的文本解析——适用于生产部署或启动延迟敏感的场景。

### Overlay（覆盖层）

多个 `.ffd` 文件可以合并，用场所特定的自定义字段扩展基线 profile。第一个 `.ffd` 提供基线，后续文件添加字段、扩展消息或覆盖字段规则：

```
# venue_extensions.ffd — 自定义字段覆盖层
field|5001|VenueOrderType|string|1
field|5002|VenueAccount|string|1
message|D|NewOrderSingle|0|5001:r,5002:o
```

### MessageBuilder 与 FixedLayoutWriter

NimbleFIX 提供两种方式构建出站消息：

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
| `nimblefix-dictgen` | 将 `.ffd` 字典编译为二进制 `.art` profile（支持合并多个 `.ffd` 文件，可生成 C++ builder 头文件） |
| `nimblefix-xml2ffd` | 将 QuickFIX XML 数据字典转换为 `.ffd` 格式；也可从 `.ffd` 生成 C++ builder 头文件 |
| `nimblefix-initiator` | CLI FIX 客户端，用于测试和互操作 |
| `nimblefix-acceptor` | CLI FIX 服务端（回显服务器） |
| `nimblefix-soak` | 带故障注入的压力测试（gap、重复、乱序、断线） |
| `nimblefix-bench` | 延迟/吞吐量测试，含分配和 CPU 计数器追踪 |
| `nimblefix-fuzz-codec` | libFuzzer，覆盖 codec 和 admin 协议 |
| `nimblefix-fuzz-config` | libFuzzer，覆盖 `.ffcfg` 配置解析 |
| `nimblefix-fuzz-dictgen` | libFuzzer，覆盖 `.ffd` 字典解析 |
| `nimblefix-interop-runner` | 双向互操作场景运行器 |

### 编译 Profile

```bash
./build/linux/x86_64/release/nimblefix-dictgen     --input samples/basic_profile.ffd     --merge samples/basic_overlay.ffd     --output build/sample-basic.art     --cpp-builders build/generated/sample_basic_builders.h
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `--input` | 是 | 基线字典文件（`.ffd`） |
| `--merge` | 否 | 额外 `.ffd` 文件（可重复指定） |
| `--output` | 是 | 输出 artifact 路径（`.art`） |
| `--cpp-builders` | 否 | 生成包含 profile 常量的 C++ 头文件 |

### 从 QuickFIX XML 转换

```bash
./build/linux/x86_64/release/nimblefix-xml2ffd     --xml FIX44.xml     --output my_profile.ffd     --profile-id 1001     --cpp-builders generated_builders.h
```

Component 会被自动内联展开，group 会被自动提取，XML 类型会自动映射到 NimbleFIX 类型，`schema_hash` 会自动计算。

---

## 用法：Initiator

### 标准 Initiator

```cpp
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/live_initiator.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/application.h"

class MyApp : public nimble::runtime::ApplicationCallbacks {
    auto OnSessionEvent(const nimble::runtime::RuntimeEvent& event)
        -> nimble::base::Status override {
        if (event.session_event == nimble::runtime::SessionEventKind::kActive) {
            // 会话已激活——开始发单
            auto msg = nimble::message::MessageBuilder{"D"}
                .set_string(11, "ORD-001")
                .set_char(54, '1')       // Side=Buy
                .set_float(44, 150.25)   // Price
                .set_int(38, 100)        // OrderQty
                .build();
            event.handle.SendTake(std::move(msg));
        }
        return nimble::base::Status::Ok();
    }

    auto OnAppMessage(const nimble::runtime::RuntimeEvent& event)
        -> nimble::base::Status override {
        auto view = event.message_view();
        auto msg_type = view.msg_type();      // "8" = ExecutionReport
        auto exec_id = view.get_string(17);   // ExecID
        return nimble::base::Status::Ok();
    }
};

int main() {
    nimble::runtime::EngineConfig config;
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

    nimble::runtime::Engine engine;
    engine.LoadProfiles(config);
    engine.Boot(config);

    auto app = std::make_shared<MyApp>();
    nimble::runtime::LiveInitiator initiator(&engine, {
        .application = app,
    });

    initiator.OpenSession(1, "exchange.example.com", 9876);
    initiator.Run();
}
```

### 热路径 Initiator：使用 FixedLayoutWriter

对于延迟敏感的下单场景，使用 `FixedLayoutWriter` 配合预构建的 `FixedLayout`：

```cpp
#include "nimblefix/message/fixed_layout_writer.h"
#include "nimblefix/codec/fix_codec.h"

// 启动时构建一次：
auto layout = nimble::message::FixedLayout::Build(dictionary, "D").value();
auto templates = nimble::codec::PrecompiledTemplateTable{};

// ... 在 OnSessionEvent 中，当 session 变为 active 时：

class HotPathApp : public nimble::runtime::ApplicationCallbacks {
    nimble::message::FixedLayout layout_;
    nimble::codec::EncodeBuffer encode_buffer_;   // 跨调用复用
    nimble::codec::EncodeOptions encode_options_;

    auto OnSessionEvent(const nimble::runtime::RuntimeEvent& event)
        -> nimble::base::Status override {
        if (event.session_event == nimble::runtime::SessionEventKind::kActive) {
            SendOrder(event.handle);
        }
        return nimble::base::Status::Ok();
    }

    void SendOrder(nimble::session::SessionHandle handle) {
        nimble::message::FixedLayoutWriter writer(layout_);
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

`FixedLayoutWriter` 路径是库里最低分配、最直接的 encode 热路径；后文 NimbleFIX vs QuickFIX 对照里的 `encode` 指标测的就是这条路径。

### 混合路径——FixedLayout + 扩展字段

当大部分字段在编译时已知、但部分字段是动态的（如场所自定义扩展）：

```cpp
nimble::message::FixedLayoutWriter writer(layout_);
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
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/live_acceptor.h"
#include "nimblefix/runtime/config.h"

nimble::runtime::EngineConfig config;
config.worker_count = 2;
config.profile_artifacts = {"build/sample-basic.art"};
config.listeners = {{
    .name = "main",
    .host = "0.0.0.0",
    .port = 9876,
}};
config.accept_unknown_sessions = true;

nimble::runtime::Engine engine;
engine.Boot(config);

// 根据入站 Logon 动态接受 session：
engine.SetSessionFactory([](const nimble::session::SessionKey& key)
    -> nimble::base::Result<nimble::runtime::CounterpartyConfig> {
    return nimble::runtime::CounterpartyConfig{
        .name = key.sender_comp_id,
        .session = {
            .profile_id = 1001,
            .key = key,
            .heartbeat_interval_seconds = 30,
        },
    };
});

auto app = std::make_shared<MyApp>();
nimble::runtime::LiveAcceptor acceptor(&engine, {
    .application = app,
});
acceptor.OpenListeners("main");
acceptor.Run();
```

### 使用预配置对端的 Acceptor

```cpp
nimble::runtime::EngineConfig config;
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
    .store_mode = nimble::runtime::StoreMode::kDurableBatch,
    .store_path = "/var/lib/nimblefix/client-a",
}};

nimble::runtime::Engine engine;
engine.LoadProfiles(config);
engine.Boot(config);

auto app = std::make_shared<MyApp>();
nimble::runtime::LiveAcceptor acceptor(&engine, {
    .application = app,
});
acceptor.OpenListeners("main");
acceptor.Run();
```

### 热路径 Acceptor：读取入站消息

在 `OnAppMessage` 回调中，使用零拷贝 `MessageView` 以最小开销读取消息：

```cpp
auto OnAppMessage(const nimble::runtime::RuntimeEvent& event)
    -> nimble::base::Status override {
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
    auto response = nimble::message::MessageBuilder{"8"}
        .set_string(17, "EXEC-001")        // ExecID
        .set_string(11, cl_ord_id.value_or(""))
        .set_char(150, '0')                // ExecType=New
        .set_int(14, 0)                    // CumQty
        .build();
    event.handle.SendTake(std::move(response));

    return nimble::base::Status::Ok();
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
| `io_backend` | enum | `kEpoll` | `kEpoll` 或 `kIoUring`（Linux I/O 后端） |
| `accept_unknown_sessions` | bool | false | 在静态 `counterparties` 未命中后，允许 `SessionFactory` 处理未知入站 Logon |
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
| `reset_seq_num_on_logon` | bool | false | 登录时重置序列号 |
| `reset_seq_num_on_logout` | bool | false | 登出时重置序列号 |
| `reset_seq_num_on_disconnect` | bool | false | 断开时重置序列号 |
| `refresh_on_logon` | bool | false | 登录前重新加载持久化恢复状态 |
| `send_next_expected_msg_seq_num` | bool | false | 在 Logon 中包含 tag 789 |
| `session_schedule` | struct | — | 会话时间窗口（开始/结束时间/日期、登录/登出窗口） |
| `day_cut` | struct | — | 日切模式和时间，用于序列号重置 |

### 工具运行时配置（`.ffcfg`）

内置二进制（`nimblefix-acceptor`、`nimblefix-interop-runner` 和测试）也支持内部用的 `.ffcfg` 格式。它本质上只是 `EngineConfig` 的便捷文本表示，不是稳定的公共库 API。

```text
engine.worker_count=1
engine.enable_metrics=true
engine.trace_mode=disabled
profile=build/sample-basic.art
listener|main|127.0.0.1|9921|0
counterparty|fix44-demo|4201|1001|FIX.4.4|SELL|BUY|memory||memory|inline|30|false
```

`profile=` 可以重复出现。`dictionary=` 也可以重复出现，并且单行 `dictionary=` 支持用逗号列出“基线 `.ffd` + overlay `.ffd`”组合，启动时会按一组字典做合并加载。

现成可运行的样例见 `tests/data/interop/loopback-runtime.ffcfg` 和 `tests/data/interop/runtime-multiversion.ffcfg`。`dispatch_mode` 控制每个 counterparty 走 inline 还是 queue-decoupled 投递；`queue_app_mode` 是 engine 级别开关，只有至少一个 counterparty 使用 `dispatch_mode=queue` 时才会生效。完整列顺序和高级字段说明见 `docs/development.md`。

---

## 线程模型

NimbleFIX 不会在你背后偷偷创建线程。每个线程都是显式配置、显式启动的，并可绑定到指定的 CPU 核心。

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

跨线程访问 session 仍然通过 `SessionHandle`，但 `SendCopy()` / `SendTake()` / `SendEncodedCopy()` / `SendEncodedTake()` 背后是每个 worker 一条 SPSC 命令队列。`Snapshot()` 和 `Subscribe()` 可以安全地从任意线程调用。发送路径严格要求单生产者：runtime 会把每个 handle 的发送队列绑定到第一次调用它的线程，如果随后有其他线程也向同一个 session 发送，会直接返回 `kInvalidArgument`，而不是静默破坏 SPSC 队列语义。

```cpp
// 可从任意线程安全调用的查询路径：
session_handle.Snapshot();
session_handle.Subscribe();

// 跨线程发送路径：
// 每个 SessionHandle 发送队列只允许一个 producer 线程。
session_handle.SendTake(std::move(msg));

// 仅限所属 worker / inline 回调作用域。
// 在 inline 回调外调用会返回 kInvalidArgument。
session_handle.SendInlineBorrowed(view);
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
config.poll_mode = nimble::runtime::PollMode::kBusy;
config.queue_app_mode = nimble::runtime::QueueAppThreadingMode::kThreaded;
```

---

## 性能测试

### 为什么和 QuickFIX 对比？

QuickFIX 仍然是大多数团队最熟悉、最常见的 C++ FIX 引擎，所以它是最有意义的参照物。NimbleFIX 把 QuickFIX 的 side-by-side 对比长期留在仓库里，确保两边都基于同一份 FIX44 业务订单夹具、同一条字典来源链路，以及同一套命令包装。

### 测试方法

- 命令：`./bench/bench.sh compare`
- 默认参数：`--iterations 100000 --loopback 1000 --replay 1000 --replay-span 128`
- 字典链路：QuickFIX `bench/vendor/quickfix/spec/FIX44.xml` → `nimblefix-xml2ffd` → `build/bench/quickfix_FIX44.ffd` → `nimblefix-dictgen` → `build/bench/quickfix_FIX44.art`
- 业务夹具：一条中性的 FIX44 `NewOrderSingle`，包含单条 `NoPartyIDs=1` repeating group
- 编码公平性：两边都把 `SendingTime` 固定为同一个夹具时间戳，让 encode 层级只比较 object-to-wire，而不是每次重新取时钟
- 分配统计：全局 `operator new` 拦截统计每轮堆分配
- CPU 计数器：在可用时通过 Linux `perf_event_open` 提供 cache miss / branch miss 列

### 当前 NimbleFIX vs QuickFIX 对照快照（2026-04-14）

| | |
|---|---|
| **CPU** | AMD Ryzen 7 7840HS with Radeon 780M Graphics |
| **操作系统** | Linux 6.19.10-1-cachyos x86_64 |
| **编译器** | `g++ (GCC) 15.2.1 20260209` |
| **构建入口** | `xmake v3.0.8+20260324` 下执行 `./bench/bench.sh compare` |

#### 共享边界对照表

| 边界 | NimbleFIX 指标 | QuickFIX 指标 | NimbleFIX p50 | NimbleFIX p95 | QuickFIX p50 | QuickFIX p95 | NimbleFIX alloc/op | QuickFIX alloc/op |
|------|--------------|---------------|-------------|-------------|--------------|--------------|------------------|-------------------|
| object → wire（复用缓冲区） | `encode` | `quickfix-encode-buffer` | 371 ns | 401 ns | 1.24 us | 1.43 us | 0 | 29 |
| wire → object | `parse` | `quickfix-parse` | 511 ns | 521 ns | 1.29 us | 1.33 us | 0 | 20 |
| session inbound | `session-inbound` | `quickfix-session-inbound` | 1.65 us | 1.94 us | 2.38 us | 2.75 us | 0 | 18 |
| replay（`replay_span=128`） | `replay` | `quickfix-replay` | 15.66 us | 16.81 us | 231.20 us | 269.07 us | 0 | 4117 |
| TCP loopback RTT | `loopback-roundtrip` | `quickfix-loopback` | 17.58 us | 20.75 us | 20.55 us | 24.68 us | 3 | 77 |

#### NimbleFIX 专有层级

| 指标 | p50 | p95 | p99 | alloc/op | ops/sec |
|------|-----|-----|-----|----------|---------|
| `peek` | 130 ns | 141 ns | 141 ns | 0 | 6.58M |

关键观察：

- 当前仓库内的 side-by-side 实测里，NimbleFIX 在所有共享边界上都领先：encode 约 3.3x、parse 约 2.5x、session-inbound 约 1.4x、replay 约 14.8x、loopback RTT 约 1.2x。
- replay 的结构差异最大：NimbleFIX 在这一层保持 0 alloc/op，而 QuickFIX 每轮大约 4117 次分配。
- loopback 是两边最接近的一层，因为消息一旦离开用户态，Linux TCP 栈本身就开始主导总耗时。
- `quickfix-encode`（fresh string）仍然会在 bench 输出里打印，但和 NimbleFIX `encode` 最接近的苹果对苹果边界是 `quickfix-encode-buffer`。

### 自行运行测试

```bash
./bench/bench.sh build
./bench/bench.sh nimblefix        # NimbleFIX 主测试（artifact）
./bench/bench.sh nimblefix-ffd    # NimbleFIX 测试（直接加载 .ffd）
./bench/bench.sh quickfix       # QuickFIX 对比测试
./bench/bench.sh builder        # 仅 object-to-wire 对比
./bench/bench.sh compare        # 完整跨引擎对比
```

上面所有 benchmark 命令都明确使用固定的 QuickFIX 4.4 输入：`bench/vendor/quickfix/spec/FIX44.xml`、`build/bench/quickfix_FIX44.ffd` 或 `build/bench/quickfix_FIX44.art`。

完整的 benchmark 方法论、每个指标的起止点、以及标明测试点在流程中位置的图，见 [bench/README.md](bench/README.md)。

---

## 更多文档

- [docs/architecture.md](docs/architecture.md) — 内部架构、模块依赖图、数据流图、线程模型细节
- [docs/development.md](docs/development.md) — 开发指南、测试、`.ffd` 格式规范、性能分析、压力/Fuzz 测试、引擎扩展
- [bench/README.md](bench/README.md) — Benchmark 基础设施、测量边界、完整结果表格

## 项目状态

NimbleFIX 正在积极开发中。核心引擎、会话管理、编解码器和持久化层已实现并测试（206 test case，2760+ assertion）。热路径编码管线持续向设计目标优化中。

## 许可证

详见 [LICENSE](LICENSE)。
