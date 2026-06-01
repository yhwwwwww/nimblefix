# NimbleFIX

**面向延迟敏感交易系统的精干、低延迟 FIX 引擎。**

[English](README.md)

---

## 项目简介

NimbleFIX 是一个用 C++20 从头实现的 FIX（Financial Information eXchange）协议引擎，专为延迟敏感的交易基础设施设计。它完整处理 FIX 会话生命周期——连接管理、Logon/Logout 握手、序列号追踪、心跳监控、gap 检测、重传恢复——同时让稳态 codec/session 路径保持低分配、单写者。

NimbleFIX 使用同一套内部引擎同时支持 **initiator**（客户端）和 **acceptor**（服务端），支持 FIX 4.2、4.3、4.4 和 FIXT.1.1。

## FIX 标准覆盖情况

NimbleFIX 的定位是聚焦经典、低延迟的 FIX 主干能力，而不是一次性覆盖整个 FIX Family of Standards。当前对 FIX Trading 主要标准族的覆盖情况如下：

| 标准族 | 状态 | 当前范围 |
|------|------|------|
| **经典 FIX session layer** | **已实现** | FIX 4.x / FIXT.1.1 的 initiator / acceptor 运行时，包含 Logon/Logout、心跳、序列号跟踪、gap 检测、重传恢复、重连与持久化 |
| **FIX tagvalue 编码** | **已实现** | 核心 codec、message builder、fixed-layout writer、raw pass-through 与 SIMD 辅助 tag/value 解析 |
| **FIX over TLS (FIXS)** | **已实现** | 基于 OpenSSL 的可选 TLS transport，可按 initiator counterparty 或 acceptor listener 在运行时启用 |
| **FIX 应用层字典** | **部分覆盖** | 以 `.nfd` / `.nfa` 为核心的字典模型，并提供 QuickFIX XML 与 FIX Orchestra XML 导入工具；Orchestra 行为规则独立落到 `.nfct` sidecar |
| **FIX 官方 session 测试案例** | **部分覆盖** | 已接入离线 FIX Trading session-case manifest 和可执行 `.nfscenario` baseline；runner 已对 72 个 mapped scenario-pass case 全部执行官方语义 predicate 机器校验，当前 0 个 partial mapping、13 个显式 unsupported |
| **FIX Orchestra** | **部分覆盖** | 已提供离线 `nimblefix-orchestra-import`，可生成结构 `.nfd`、`.nfct` contract sidecar、dump/markdown/interop augmentations，并对未支持语义显式告警 |
| **FIXP / SOFH** | **未实现** | 尚未支持 FIX Performance Session Layer 或 Simple Open Framing Header |
| **FIXML / SBE / FAST** | **未实现** | 目前没有 classic tag=value 之外的其他 wire encoding |
| **JSON / GPB / ASN.1 FIX 编码** | **未实现** | 尚未实现其他序列化形式的 FIX 编码 |
| **FIXatdl / MMT** | **未实现** | 当前不在引擎/runtime/tooling 的范围内 |

## 为什么做这个项目？

现有的开源 FIX 引擎（QuickFIX、QuickFIX/J、Fix8）以正确性和广泛兼容性为设计目标，而非极致速度。它们把消息解析到动态 map 中，每条消息都做堆分配，用锁保护跨线程共享状态。这对 99% 的场景够用——但对微秒级延迟有刚需的团队来说不够。

NimbleFIX 的设计出发点是：*如果每个设计决策都为热路径优化，会怎样？*

- **字典支撑编解码**：FIX 元数据会在启动时归一化到内存中，来源既可以是 `.nfd` 文本字典，也可以是预编译的 `.nfa` artifact。两条路径最终都进入同一套运行时字典表示。
- **零拷贝消息视图**：解析后的消息是对原始字节缓冲区的轻量视图，不拷贝，除非你显式请求。
- **单写者会话模型**：每个 session 由且仅由一个 worker 线程持有。协议状态避免共享 session 锁，跨线程交接使用 worker-local SPSC 队列。
- **预编译帧模板**：高频消息类型的 header/trailer 片段预先构建，每条消息只需填充可变字段、BodyLength 和 Checksum。
- **SIMD 加速解析**：tokenizer 热循环中使用 SSE2 向量化扫描 SOH 分隔符和 `=` 分隔符。

## 亮点

| 特性 | 说明 |
|------|------|
| **编解码延迟** | 当前本机 FIX44 对比结果：header peek 100 ns (p50)，完整解析 591 ns (p50) |
| **低分配压力** | 当前本机 compare 结果：parse/inbound/replay 为 0 alloc/op；outbound 为 1 alloc/op；loopback 为 3 alloc/op |
| **会话管理覆盖** | 覆盖核心 Logon/Logout/Heartbeat/TestRequest/ResendRequest/SequenceReset 路径 |
| **嵌套重复组** | 通过字典元数据完整支持嵌套 repeating group |
| **自动重连退避** | initiator 可配置指数退避 + 随机抖动的自动重连 |
| **可选 TLS 传输** | 可编译 OpenSSL 支持，并按 initiator counterparty 或 acceptor listener 在运行时启用 |
| **动态会话工厂** | acceptor 可通过回调或白名单接纳未知 CompID |
| **可插拔持久化** | Memory、mmap、durable batch store，支持可配置 rollover |
| **Worker 分片** | 每 worker 独立事件循环，支持 CPU 亲和性绑定 |
| **Busy-poll 模式** | 可选零超时轮询，实现最低延迟 |
| **SIMD tokenizer** | SSE2 字节扫描，自动回退到标量路径 |
| **可观测性** | 内置 metrics 注册表和环形缓冲 trace 记录器 |
| **压力测试** | 故障注入工具（gap、重复、乱序、断线重连） |
| **Fuzz 测试** | codec/admin、config、dictionary 的独立 corpus runner，以及 codec libFuzzer 入口 |
| **高可用** | Active/standby HA，支持可配置的故障转移、心跳监控和 callback 驱动状态复制 |
| **动态配置** | 通过 `ApplyConfig()` 热更新 counterparty 和部分引擎字段；listener socket 变更需要重新打开 listener |
| **预热** | 在正式流量前显式运行 codec 和 profile 路径预热步骤 |
| **诊断** | 通过 `DiagnosticsMonitor` 获取引擎健康快照，支持可插拔 sink（JSON、文本） |
| **管理平面** | 运行时查询引擎/会话状态和健康快照 |
| **消息日志** | 导出和回放已存储的 FIX 消息，支持可配置速度（最大、实时、单步） |
| **连接策略** | 可插拔的重连策略，支持备用端点故障转移 |
| **消息路由** | 基于表达式的路由表，支持负载均衡、字段转换和转发桥接 |
| **会话调度** | 时间窗口会话控制，支持 logon/logout 窗口、星期几和多段 schedule |
| **Schema 优化器** | 分析实时流量以裁剪未使用的 tag，评估 FixedLayout 内存节省 |
| **消息 dump** | 将原始 FIX 消息格式化和过滤为可读文本或 JSON |
| **时间戳精度** | 每会话可配置 SendingTime 精度：秒、毫秒、微秒或纳秒 |
| **验证回调** | 每会话可插拔的验证钩子，用于自定义入站消息检查 |

## 与 QuickFIX 的差异

[QuickFIX](http://www.quickfixengine.org/) 是事实上的开源 FIX 引擎标准。它成熟、广泛使用、正确。NimbleFIX 在设计理念上有根本不同：

| 方面 | QuickFIX | NimbleFIX |
|------|----------|---------|
| **解析** | 运行时加载 XML 数据字典；字段存入 `std::map` | 启动时从 `.nfa` 或 `.nfd` 归一化到内存字典；热路径使用连续查找表 |
| **消息表示** | `FieldMap`，堆分配字符串 | 零拷贝 `MessageView`，直接引用原始字节 |
| **每消息分配** | 多次（map 插入、字符串拷贝） | 复用缓冲区编码路径为 0 次 |
| **线程模型** | 全局 session 锁，thread-per-session | 每 session 单写者、分片 worker、SPSC 交接 |
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

TLS 支持是可选能力。构建时打开它只表示链接 OpenSSL 并编译 TLS transport；连接默认仍是明文 TCP，只有运行时配置里的 `enabled=true` 才会启用 TLS。

```bash
# CMake TLS-capable build
cmake -S . -B build/cmake/tls-release -DCMAKE_BUILD_TYPE=Release -DNIMBLEFIX_ENABLE_TLS=ON
cmake --build build/cmake/tls-release

# xmake TLS-capable build
xmake f -m release --nimblefix_enable_tls=true --ccache=n -y
xmake build nimblefix-tests
```

辅助脚本现在会按 `xmake >= 3.0.0`、`cmake + Ninja`、`cmake + make` 的顺序自动选择。Ubuntu 24.04 仓库里的 xmake 目前只有 `2.8.7`，因此 helper 会在那里明确打印并回退到 CMake，除非你额外安装更高版本的 xmake。两个 helper 在 xmake 路径下也默认 `NIMBLEFIX_XMAKE_CCACHE=n`，用于规避 Linux 上 `.build_cache/... file busy` 的已知问题。

GitHub Actions CI 现在就在 Ubuntu 上使用这套自动选择逻辑，同时继续通过 `ubi8/ubi:8.10` + `gcc-toolset-12` 和 `ubi9/ubi:9.7` + `gcc-toolset-14` 容器 job 实际跑这两个 RHEL CMake preset。

如果 `build/generated/`、`build/bench/` 或 `build/sample-basic.nfa` 被删掉了，`xmake build nimblefix-tests` 和 `xmake build nimblefix-bench` 现在会自动把这些共享资产重新生成出来。

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
2. **编译协议 Profile**：运行 `nimblefix-dictgen` 从字典文件生成 `.nfa` 二进制 artifact（可选，也可在运行时直接加载 `.nfd`）
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

- 生成出来的 profile 头，例如 `fix44_api.h`
- `nimblefix/runtime/config.h`
- `nimblefix/runtime/engine.h`
- `nimblefix/runtime/initiator.h` 或 `nimblefix/runtime/acceptor.h`
- `nimblefix/runtime/profile_binding.h`
- `nimblefix/message/message_view.h`
- `nimblefix/codec/fix_codec.h`
- `nimblefix/profile/profile_loader.h`
- `nimblefix/store/memory_store.h`
- `nimblefix/store/mmap_store.h`
- `nimblefix/store/durable_batch_store.h`

高级用法再按需补 `nimblefix/advanced/runtime_application.h`、`nimblefix/advanced/engine.h`、`nimblefix/advanced/message_builder.h`、`nimblefix/advanced/encoded_application_message.h`、`nimblefix/advanced/session_handle.h`、`nimblefix/session/admin_protocol.h`、`nimblefix/session/resend_recovery.h`、`nimblefix/runtime/sharded_runtime.h`、`nimblefix/runtime/metrics.h`、`nimblefix/runtime/trace.h`、`nimblefix/runtime/ha.h`、`nimblefix/runtime/dynamic_config.h`、`nimblefix/runtime/warmup.h`、`nimblefix/runtime/diagnostics.h`、`nimblefix/runtime/management.h`、`nimblefix/runtime/message_log.h`、`nimblefix/runtime/connection_strategy.h`、`nimblefix/runtime/session_schedule.h`、`nimblefix/runtime/router.h`、`nimblefix/runtime/io_backend.h`、`nimblefix/session/validation_callback.h`、`nimblefix/codec/timestamp_resolution.h`、`nimblefix/tools/schema_optimizer.h`、`nimblefix/tools/message_dump.h`。`FixedLayoutWriter` 现在仅供仓库内部使用。完整的导出头策略见 [docs/public-api.md](docs/public-api.md)。

---

## 核心概念

### Profile

**Profile** 是一个 FIX 协议变体的编译后二进制描述——包含哪些字段、定义了哪些消息、消息里有什么 group。每个 profile 携带一个你自己指定的 `profile_id`（uint64）。Session 通过这个 ID 引用 profile，一个 Engine 可以同时加载多个 profile（如 FIX 4.2 和 FIX 4.4）。

### `.nfd` — NimbleFIX 字典

文本文件，定义一个 FIX 变体的全部字段、消息和 group。其中包含你指定的 `profile_id`（任意唯一数字，例如 `1001` 代表 FIX 4.4）。可向 `nimblefix-dictgen` 或 Engine 传入多个 `.nfd` 文件——第一个提供基线定义，其余文件作为增量覆盖层。

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

### `.nfa` — Artifact

`nimblefix-dictgen` 的编译输出。扁平的、可 mmap 加载的二进制文件，包含字符串表、字段/消息/group 定义、校验规则和查找表。`.nfd` 中的 `profile_id` 会嵌入其中。加载 `.nfa` 避免启动时的文本解析——适用于生产部署或启动延迟敏感的场景。

### `.nfct` — Contract Sidecar

由 `nimblefix-orchestra-import` 生成的行为 companion artifact。`.nfct` 被刻意设计成与 `.nfa` 分离：`.nfa` 继续保持结构字典、mmap 友好、服务热路径的定位；`.nfct` 只承载冷路径行为信息，例如条件必填/禁填字段、枚举或代码值约束、角色与方向限制、service subset、flow edge、源 rule id 以及 importer warning。运行时通过 `EngineConfig::profile_contracts` 加载 `.nfct`，并通过 `CounterpartyConfig::contract_service_subsets` 选择部署要启用的 service subset。暂不支持的 Orchestra 语义会以 importer warning 的形式保留，不会被解释进 steady-state codec/session 热路径。

### Overlay（覆盖层）

多个 `.nfd` 文件可以合并，用场所特定的自定义字段扩展基线 profile。第一个 `.nfd` 提供基线，后续文件添加字段、扩展消息或覆盖字段规则：

```
# venue_extensions.nfd — 自定义字段覆盖层
field|5001|VenueOrderType|string|1
field|5002|VenueAccount|string|1
message|D|NewOrderSingle|0|5001:r,5002:o
```

### Generated API 与高级 Raw Surface

常规业务流程优先使用 `--cpp-api` 生成的消息对象，以及 `runtime::Session<Profile>` / `runtime::InlineSession<Profile>`。

更底层的 escape hatch 仍然保留，但它们不再是主业务 API：

- **动态/raw 消息构造能力** — 面向工具、协议桥或 schema-agnostic 场景，放在 `nimblefix/advanced/` 下。
- **Fixed-layout 编码后端** — `FixedLayoutWriter` 现在是仓库内部实现细节；外部代码应使用生成的 `send<Msg>` 或 `advanced/` 下的动态/raw builder。

### Raw MessageView

生成的 inbound view 是默认业务读路径。`MessageView` 继续作为 schema-agnostic 检查和协议工具场景下的零拷贝 raw 访问器。

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
| `nimblefix-dictgen` | 将 `.nfd` 字典编译为二进制 `.nfa` profile（支持合并多个 `.nfd` 文件，可生成 C++ typed API 头文件） |
| `nimblefix-xml2nfd` | 将 QuickFIX XML 数据字典转换为 `.nfd` 格式；也可从 `.nfd` 生成 C++ typed API 头文件 |
| `nimblefix-orchestra-import` | 将 FIX Orchestra XML 转换为结构 `.nfd` 和 `.nfct` contract sidecar；也可对 sidecar 做 dump、markdown 导出和 interop augmentation 生成 |
| `nimblefix-initiator` | CLI FIX 客户端，用于测试和互操作；属于运维/高级工具，不是主 generated-first 应用示例 |
| `nimblefix-acceptor` | CLI FIX 服务端（回显服务器）；属于运维/高级工具，不是主 generated-first 应用示例 |
| `nimblefix-soak` | 带故障注入的压力测试（gap、重复、乱序、断线） |
| `nimblefix-bench` | 延迟/吞吐量测试，含分配和 CPU 计数器追踪 |
| `nimblefix-msgdump` | 将存储或原始 FIX 消息格式化、过滤为文本或 JSON |
| `nimblefix-router` | 对原始 FIX 输入应用路由规则并执行转发变换 |
| `nimblefix-schema-optimizer` | 分析观测消息并估算裁剪 schema 后的布局节省 |
| `nimblefix-fuzz-codec` | 独立 codec/admin corpus runner；`nimblefix-fuzz-codec-libfuzzer` 是 codec libFuzzer 入口 |
| `nimblefix-fuzz-config` | `.nfcfg` 配置解析的独立 corpus runner |
| `nimblefix-fuzz-dictgen` | `.nfd` 字典解析的独立 corpus runner |
| `nimblefix-usagegen` | 扫描 `send<Msg>(populate, extras)` 调用点，生成 canonical `MessageShape`、去重并输出 specialized send-site 分析 |
| `nimblefix-interop-runner` | 双向互操作场景运行器 |

### 编译 Profile

```bash
./build/linux/x86_64/release/nimblefix-dictgen     --input samples/basic_profile.nfd     --merge samples/basic_overlay.nfd     --output build/sample-basic.nfa     --cpp-api build/generated/sample_basic_api.h
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `--input` | 是 | 基线字典文件（`.nfd`） |
| `--merge` | 否 | 额外 `.nfd` 文件（可重复指定） |
| `--output` | 是 | 输出 artifact 路径（`.nfa`） |
| `--cpp-api` | 否 | 生成包含 typed generated API 的 C++ 头文件 |

### 从 QuickFIX XML 转换

```bash
./build/linux/x86_64/release/nimblefix-xml2nfd     --xml FIX44.xml     --output my_profile.nfd     --profile-id 1001     --cpp-api generated_api.h
```

Component 会被自动内联展开，group 会被自动提取，XML 类型会自动映射到 NimbleFIX 类型，`schema_hash` 会自动计算。

### 从 FIX Orchestra XML 导入

```bash
./build/linux/x86_64/release/nimblefix-orchestra-import \
    --xml FIXOrchestra.xml \
    --profile-id 4400 \
    --output-nfd build/fix44-orchestra.nfd \
    --output-contract build/fix44-orchestra.nfct

./build/linux/x86_64/release/nimblefix-dictgen \
    --input build/fix44-orchestra.nfd \
    --output build/fix44-orchestra.nfa
```

可以用 `--contract <file> --dump`、`--markdown <file>` 或 `--interop-dir <dir>` 检查生成的 sidecar，并导出 contract 驱动的 `.nfscenario` augmentation。暂不支持的 Orchestra 规则会保留为 warning，而不是被解释进运行时热路径。

---

## 用法：Initiator

### 标准 Initiator

```cpp
#include <memory>

#include "fix44_api.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/initiator.h"
#include "nimblefix/runtime/profile_binding.h"

using namespace nimble::generated::profile_4400;

class MyApp final : public Handler {
public:
    auto OnSessionActive(nimble::runtime::Session<Profile>& session)
        -> nimble::base::Status override {
        return session.send<NewOrderSingle>([](auto& order) {
            order.cl_ord_id("ORD-001")
                .symbol("AAPL")
                .side(Side::Buy)
                .transact_time("20260429-09:30:00.000")
                .order_qty(100)
                .ord_type(OrdType::Limit)
                .price(150.25);
        });
    }

    auto OnExecutionReport(nimble::runtime::InlineSession<Profile>&,
                           ExecutionReportView exec)
        -> nimble::base::Status override {
        auto exec_id = exec.exec_id_raw();
        auto ord_status = exec.ord_status();
        (void)exec_id;
        (void)ord_status;
        return nimble::base::Status::Ok();
    }
};

int main() {
    nimble::runtime::EngineConfig config;
    config.worker_count = 1;
    config.profile_artifacts = {"build/bench/quickfix_FIX44.nfa"};
    config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
        .name = "venue-a",
        .session = {
            .session_id = 1U,
            .key = nimble::session::SessionKey::ForInitiator("MY_FIRM", "VENUE_A"),
            .profile_id = Profile::kProfileId,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = true,
        },
        .transport_profile = nimble::session::TransportSessionProfile::Fix44(),
        .reconnect_enabled = true,
        .reconnect_initial_ms = nimble::runtime::kDefaultReconnectInitialMs,
        .reconnect_max_ms = nimble::runtime::kDefaultReconnectMaxMs,
        .reconnect_max_retries = nimble::runtime::kUnlimitedReconnectRetries,
    });

    nimble::runtime::Engine engine;
    auto boot = engine.Boot(config);
    if (!boot.ok()) {
        return 1;
    }

    auto binding = engine.Bind<Profile>();
    if (!binding.ok()) {
        return 1;
    }

    auto app = std::make_shared<MyApp>();
    nimble::runtime::Initiator<Profile> initiator(&engine, &binding.value(), { .application = app });

    auto open = initiator.OpenSession(1U, "exchange.example.com", 9876);
    if (!open.ok()) {
        return 1;
    }
    return initiator.Run().ok() ? 0 : 1;
}
```

高级 raw 编码和动态消息 escape hatch 仍然保留在 `nimblefix/advanced/` 下，但它们被刻意放在主教程之外。只有当你明确需要 raw builder、pre-encoded outbound payload 或直接访问 raw send handle 时，再去看 [docs/public-api.md](docs/public-api.md)。

---

## 用法：Acceptor

### 使用动态 Session 工厂的标准 Acceptor

```cpp
#include <memory>

#include "fix44_api.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/acceptor.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/profile_binding.h"

using namespace nimble::generated::profile_4400;

class MyApp final : public Handler {
public:
    auto OnNewOrderSingle(nimble::runtime::InlineSession<Profile>& session,
                          NewOrderSingleView order) -> nimble::base::Status override {
        return session.send<ExecutionReport>([&](auto& report) {
            report.order_id("ORD-001")
                .exec_id("EXEC-001")
                .exec_type(ExecType::New)
                .ord_status(OrdStatus::New)
                .side(order.side().value())
                .leaves_qty(order.order_qty().value_or(0))
                .cum_qty(0)
                .avg_px(0.0);
            if (auto cl_ord_id = order.cl_ord_id(); cl_ord_id.has_value()) {
                report.cl_ord_id(*cl_ord_id);
            }
            if (auto symbol = order.symbol(); symbol.has_value()) {
                report.symbol(*symbol);
            }
        });
    }
};

nimble::runtime::EngineConfig config;
config.worker_count = 2;
config.profile_artifacts = {"build/bench/quickfix_FIX44.nfa"};
config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "main",
    .host = "0.0.0.0",
    .port = 9876,
});
config.accept_unknown_sessions = true;

nimble::runtime::Engine engine;
auto boot = engine.Boot(config);
if (!boot.ok()) {
    return 1;
}

// 根据入站 Logon 动态接受 session：
engine.SetSessionFactory([](const nimble::session::SessionKey& key)
    -> nimble::base::Result<nimble::runtime::CounterpartyConfig> {
    return nimble::runtime::CounterpartyConfig{
        .name = key.sender_comp_id,
        .session = {
            .profile_id = Profile::kProfileId,
            .key = key,
            .heartbeat_interval_seconds = 30,
        },
    };
});

auto binding = engine.Bind<Profile>();
if (!binding.ok()) {
    return 1;
}

auto app = std::make_shared<MyApp>();
nimble::runtime::Acceptor<Profile> acceptor(&engine, &binding.value(), { .application = app });
auto open = acceptor.OpenListeners("main");
if (!open.ok()) {
    return 1;
}
return acceptor.Run().ok() ? 0 : 1;
```

### 使用预配置对端的 Acceptor

```cpp
using namespace nimble::generated::profile_4400;

nimble::runtime::EngineConfig config;
config.worker_count = 1;
config.profile_artifacts = {"build/bench/quickfix_FIX44.nfa"};
config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "main",
    .host = "0.0.0.0",
    .port = 9876,
});
config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "client-a",
    .session = {
        .session_id = 1,
        .key = nimble::session::SessionKey::ForAcceptor("SERVER", "CLIENT_A"),
        .profile_id = Profile::kProfileId,
        .heartbeat_interval_seconds = 30,
        .is_initiator = false,
    },
    .transport_profile = nimble::session::TransportSessionProfile::Fix44(),
    .store_mode = nimble::runtime::StoreMode::kDurableBatch,
    .store_path = "/var/lib/nimblefix/client-a",
});

nimble::runtime::Engine engine;
auto boot = engine.Boot(config);
if (!boot.ok()) {
    return 1;
}

auto binding = engine.Bind<Profile>();
if (!binding.ok()) {
    return 1;
}

auto app = std::make_shared<MyApp>();
nimble::runtime::Acceptor<Profile> acceptor(&engine, &binding.value(), { .application = app });
auto open = acceptor.OpenListeners("main");
if (!open.ok()) {
    return 1;
}
return acceptor.Run().ok() ? 0 : 1;
```

### 热路径 Acceptor：读取入站消息

在 typed handler 路径里，常规业务逻辑直接使用生成的 inbound view；只有做协议工具或网关时才退回 raw `MessageView`：

```cpp
auto OnNewOrderSingle(nimble::runtime::InlineSession<Profile>& session,
                      NewOrderSingleView order)
    -> nimble::base::Status override {
    if (auto parties = order.parties(); parties.has_value()) {
        for (const auto party : *parties) {
            auto party_id = party.party_id();
            auto party_role = party.party_role();
            (void)party_id;
            (void)party_role;
        }
    }

    return session.send<ExecutionReport>([&](auto& report) {
        report.order_id("ORD-001")
            .exec_id("EXEC-001")
            .exec_type(ExecType::New)
            .ord_status(OrdStatus::New)
            .side(order.side().value())
            .leaves_qty(order.order_qty().value_or(0))
            .cum_qty(0)
            .avg_px(0.0);
    });
}
```

---

## 配置参考

### EngineConfig（引擎配置）

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `worker_count` | uint32 | 1 | worker shard 数；大于 1 时启动 worker 线程 |
| `poll_mode` | enum | `kBlocking` | `kBlocking`（epoll 带超时）或 `kBusy`（自旋轮询，最低延迟） |
| `enable_metrics` | bool | true | 启用内置指标收集 |
| `trace_mode` | enum | `kDisabled` | `kDisabled` 或 `kRing`（环形缓冲 trace） |
| `trace_capacity` | uint32 | 0 | 环形 trace 缓冲区大小 |
| `profile_artifacts` | paths | — | 编译好的 `.nfa` profile 文件路径 |
| `profile_dictionaries` | path[][] | — | 运行时直接加载的 `.nfd` 字典文件组（每个内层向量作为基线 + 覆盖层合并） |
| `profile_contracts` | paths | — | 冷路径加载的 `.nfct` contract sidecar 路径，按 `profile_id` 匹配 |
| `profile_madvise` | bool | false | 对加载的 artifact 调用 `madvise(MADV_WILLNEED)` |
| `profile_mlock` | bool | false | 对加载的 artifact 调用 `mlock()`（需足够的 RLIMIT） |
| `front_door_cpu` | uint32? | — | 绑定 acceptor 前门线程到指定 CPU 核 |
| `worker_cpu_affinity` | uint32[] | — | 绑定 worker 线程到 CPU 核 |
| `queue_app_mode` | enum | `kCoScheduled` | `kCoScheduled`（在 worker 线程上 drain）或 `kThreaded`（每 worker 一个专用 app 线程） |
| `app_cpu_affinity` | uint32[] | — | 绑定 app 线程到 CPU 核（`kThreaded` 时） |
| `io_backend` | enum | `kEpoll` | `kEpoll` 或可用时的 `kIoUring`（Linux I/O 后端） |
| `accept_unknown_sessions` | bool | false | 在静态 `counterparties` 未命中后，允许 `SessionFactory` 处理未知入站 Logon |
| `listeners` | list | — | TCP 侦听配置（仅 acceptor） |
| `backlog_warn_threshold_ms` | uint32 | 5000 | outbound command 在 worker 队列等待超过该阈值时输出 warning（ms） |
| `backlog_warn_throttle_ms` | uint32 | 1000 | 同一连接 backlog warning 的最小间隔（ms） |
| `counterparties` | list | — | 预配置的 session 对端 |

### ListenerConfig（侦听配置）

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `name` | string | — | 侦听器标识（用于 `OpenListeners()` 调用） |
| `host` | string | `"0.0.0.0"` | 绑定地址 |
| `port` | uint16 | 0 | 侦听端口 |
| `worker_hint` | uint32 | 0 | Worker 池路由提示 |
| `tls_server` | TlsServerConfig | disabled | 此 listener 的可选 TLS server 策略 |

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
| `supported_app_msg_types` | string[] | empty | 可选的入站应用层 MsgType allowlist；若绑定了 contract sidecar，则它必须是该 contract 接收子集的子集 |
| `contract_service_subsets` | string[] | empty | 可选的 Orchestra service subset 选择；仅在存在匹配的 `.nfct` sidecar 时有效 |
| `application_messages_available` | bool | true | 为 false 时，已知应用消息会收到 `BusinessMessageReject(380=4)` |
| `store_mode` | enum | `kMemory` | `kMemory`、`kMmap` 或 `kDurableBatch` |
| `store_path` | path | — | mmap/durable store 文件目录 |
| `recovery_mode` | enum | `kMemoryOnly` | `kMemoryOnly`、`kWarmRestart`、`kColdStart` 或 `kNoRecovery` |
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
| `tls_client` | TlsClientConfig | disabled | initiator 连接的可选 TLS client 策略 |
| `acceptor_transport_security` | enum | `kAny` | acceptor 侧要求：任意、仅明文或仅 TLS |
| `sending_time_threshold_seconds` | uint32 | 0 | SendingTime 允许的最大时钟偏差（0 = 禁用） |
| `warmup_message_count` | uint32 | 0 | session 激活后标记为 warmup 的入站应用消息数量 |
| `timestamp_resolution` | enum | `kMilliseconds` | SendingTime 精度：`kSeconds`、`kMilliseconds`、`kMicroseconds` 或 `kNanoseconds` |
| `validation_callback` | shared_ptr | — | 可选的每会话回调，用于自定义入站消息验证 |
| `connection_strategy` | shared_ptr | — | 可选的可插拔重连策略（覆盖指数退避） |
| `alternate_endpoints` | list | — | initiator 重连的故障转移端点（由连接策略使用） |

### TLS 运行时配置

TLS 在 FIX Logon 之前完成握手。因此 acceptor TLS 配置属于 `ListenerConfig`，initiator TLS 配置属于 initiator `CounterpartyConfig`。TLS 不改变 `SessionKey`、序列号、store key、profile 选择或 replay 语义。

```cpp
nimble::runtime::TlsServerConfig server_tls;
server_tls.enabled = true;
server_tls.certificate_chain_file = "/etc/nimblefix/server-chain.pem";
server_tls.private_key_file = "/etc/nimblefix/server-key.pem";
server_tls.ca_file = "/etc/nimblefix/client-ca.pem";
server_tls.verify_peer = true;
server_tls.require_client_certificate = true;

config.listeners.push_back(
    nimble::runtime::ListenerConfig{
        .name = "tls-main",
        .host = "0.0.0.0",
        .port = 9877,
        .tls_server = std::move(server_tls),
    });

nimble::runtime::TlsClientConfig client_tls;
client_tls.enabled = true;
client_tls.server_name = "fix.example.com";
client_tls.expected_peer_name = "fix.example.com";
client_tls.ca_file = "/etc/nimblefix/ca.pem";
client_tls.min_version = nimble::runtime::TlsProtocolVersion::kTls12;

config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "buy-side-tls",
    .session = {
        .session_id = 1001U,
        .key = nimble::session::SessionKey::ForInitiator("BUY1", "SELL1"),
        .profile_id = 4400U,
        .heartbeat_interval_seconds = 30U,
        .is_initiator = true,
    },
    .transport_profile = nimble::session::TransportSessionProfile::Fix44(),
    .tls_client = std::move(client_tls),
});
```

对 acceptor，如果敏感 session 不能绑定到明文 listener，可配置 `acceptor_transport_security(kTlsOnly)`。`verify_peer=false` 只适合受控测试环境；生产部署应启用 peer verification，并提供 CA 文件或目录。

### 工具运行时配置（`.nfcfg`）

内置二进制（`nimblefix-acceptor`、`nimblefix-interop-runner` 和测试）也支持内部用的 `.nfcfg` 格式。它本质上只是 `EngineConfig` 的便捷文本表示，不是稳定的公共库 API。

```text
engine.worker_count=1
engine.enable_metrics=true
engine.trace_mode=disabled
profile=build/sample-basic.nfa
contract=build/sample-basic.nfct
listener|main|127.0.0.1|9921|0
counterparty|fix44-demo|4201|1001|FIX.4.4|SELL|BUY|memory||memory|inline|30|false
```

`profile=` 可以重复出现。`contract=` 也可以重复出现，用来声明按 `profile_id` 匹配的 `.nfct` sidecar。`dictionary=` 同样可以重复出现，并且单行 `dictionary=` 支持用逗号列出“基线 `.nfd` + overlay `.nfd`”组合，启动时会按一组字典做合并加载。

counterparty 记录在尾部还支持 Phase 12 新列：`sending_time_threshold_seconds|supported_app_msg_types|application_messages_available|contract_service_subsets`。其中 `contract_service_subsets` 是逗号分隔的 service 名称列表，用于从已加载的 contract sidecar 中选择部署要开放的接收入站应用消息子集。

现成可运行的样例见 `tests/data/interop/loopback-runtime.nfcfg` 和 `tests/data/interop/runtime-multiversion.nfcfg`。`dispatch_mode` 控制每个 counterparty 走 inline 还是 queue-decoupled 投递；`queue_app_mode` 是 engine 级别开关，只有至少一个 counterparty 使用 `dispatch_mode=queue` 时才会生效。完整列顺序和高级字段说明见 `docs/development.md`。

---

## 线程模型

NimbleFIX 不会在你背后偷偷创建线程。每个线程都是显式配置、显式启动的，并可绑定到指定的 CPU 核心。

### 线程拓扑

**Acceptor（服务端）：**

| 线程 | 命名 | 数量 | 职责 | CPU 绑定 |
|------|------|------|------|---------|
| 前门 | `nf-acc-main` | 1 | accept TCP 连接 → 读 Logon → 路由到负载最低的 worker | `front_door_cpu` |
| 会话 worker | `nf-acc-w{N}` | `worker_count` | 独占 session：解码、序号、定时器、编码、发送 | `worker_cpu_affinity[N]` |
| 应用 worker | `nf-app-w{N}` | `worker_count`（仅 `queue_app_mode=kThreaded`） | 消费 SPSC 队列，调用业务回调 | `app_cpu_affinity[N]` |

**Initiator（客户端）：** 当 `worker_count>1` 时，调用者线程命名为 `nf-ini-main` 并协调运行循环；会话 worker 命名为 `nf-ini-w{N}`。它没有 acceptor 风格的 TCP 前门线程。

`worker_count=1` 时，唯一的 worker 直接在调用者线程上运行——不会额外创建线程。应用 worker 仅在 `queue_app_mode = kThreaded` 时存在。

### Session 所有权

每个 session 严格归属一个 worker 线程。该 worker 独占处理所有协议状态：解码、序号管理、定时器、持久化、编码和发送。**热路径无锁。**

常规应用代码应优先使用 typed 的 `runtime::Session<Profile>` / `runtime::InlineSession<Profile>`。它们的 `send()` 和 `snapshot()` 才是唯一推荐的业务路径。

底层仍然是同一条 single-producer command bridge。只有在你明确下沉到 `advanced/` API 时，raw handle 规则才需要直接关心：query 路径可以从任意线程安全调用，owned send ref 底下仍然是每个 worker 一条 SPSC 队列，而 borrowed ref 仍然只是 callback 作用域内的 escape hatch。一个 typed session 或 raw handle 的发送路径都应视为单生产者。

```cpp
session.snapshot();
session.send<NewOrderSingle>([](auto& order) { /* populate order */ });
```

### 应用回调模式

| 模式 | 你的代码在哪运行 | 约束 |
|------|-----------------|------|
| `kInline`（默认） | 在会话 worker 线程内，I/O 循环中 | 不能阻塞 |
| `kQueueDecoupled` + `kCoScheduled` | 在会话 worker 线程内，显式 poll 调用时 | 不能阻塞 |
| `kQueueDecoupled` + `kThreaded` | 在专用应用 worker 线程（`nf-app-wN`）上 | 可以阻塞（适度） |

**Inline 模式**延迟最低。generated-first 路径下，runtime 会直接在 session worker 线程上分派 typed inbound view；高级 raw 集成仍然可以直接消费零拷贝 `MessageView`。无论哪种模式，回调都必须快速返回，任何阻塞都会卡住该 worker 上的所有 session。

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
- 字典链路：QuickFIX `bench/vendor/quickfix/spec/FIX44.xml` → `nimblefix-xml2nfd` → `build/bench/quickfix_FIX44.nfd` → `nimblefix-dictgen` → `build/bench/quickfix_FIX44.nfa`
- 业务夹具：一条中性的 FIX44 `NewOrderSingle`，包含单条 `NoPartyIDs=1` repeating group
- 编码公平性：两边都把 `SendingTime` 固定为同一个夹具时间戳，确保延迟测量不包含时钟格式化开销
- 分配统计：全局 `operator new` 拦截统计每轮堆分配
- CPU 计数器：在可用时通过 Linux `perf_event_open` 提供 cache miss / branch miss 列

### 当前本机 NimbleFIX vs QuickFIX 对照结果（2026-05-30）

| | |
|---|---|
| **CPU** | AMD Ryzen 7 7840HS with Radeon 780M Graphics |
| **操作系统** | Linux 7.0.9-1-cachyos x86_64 |
| **编译器** | `g++ (GCC) 16.1.1 20260430` |
| **构建入口** | `xmake v3.0.9+20260519` 下执行 `./bench/bench.sh compare` |

#### 共享边界对照表

| 边界 | NimbleFIX 指标 | QuickFIX 指标 | NimbleFIX p50 | NimbleFIX p95 | QuickFIX p50 | QuickFIX p95 | NimbleFIX alloc/op | QuickFIX alloc/op |
|------|---------------|--------------|-------------|-------------|-------------|-------------|-------------------|-------------------|
| 用户编码 | `encode` | `quickfix-encode` | 391 ns | 410 ns | 1.41 us | 1.44 us | 0.0 | 29.0 |
| 会话出站 | `outbound` | `quickfix-outbound` | 712 ns | 751 ns | 1.70 us | 2.95 us | 1.0 | 33 |
| 线格式 → 对象 | `parse` | `quickfix-parse` | 591 ns | 611 ns | 1.48 us | 1.51 us | 0 | 20.0 |
| 会话入站 | `inbound` | `quickfix-inbound` | 1.22 us | 1.30 us | 2.46 us | 2.54 us | 0 | 12 |
| 重放 (`replay_span=128`) | `replay` | `quickfix-replay` | 14.45 us | 15.43 us | 265.54 us | 293.58 us | 0 | 4117.0 |
| TCP 回环往返 | `loopback` | `quickfix-loopback` | 17.17 us | 18.68 us | 21.86 us | 24.62 us | 3.0 | 77.0 |

#### NimbleFIX 专有层级

| 指标 | p50 | p95 | p99 | alloc/op | ops/sec |
|------|-----|-----|-----|----------|---------|
| `peek` | 100 ns | 101 ns | 101 ns | 0 | 8.65M |

关键观察：

- 在这次本机 side-by-side 实测里，NimbleFIX 在所有共享边界上都领先：encode 约 3.6x、outbound 约 2.4x、parse 约 2.5x、inbound 约 2.0x、replay 约 18.4x、loopback RTT 约 1.3x。
- replay 的结构差异最大：NimbleFIX 在这一层保持 0 alloc/op，而 QuickFIX 每轮大约 4117 次分配。
- loopback 是两边最接近的一层，因为消息一旦离开用户态，Linux TCP 栈本身就开始主导总耗时。

### 自行运行测试

```bash
./bench/bench.sh build
./bench/bench.sh nimblefix        # NimbleFIX 主测试（artifact）
./bench/bench.sh nimblefix-nfd    # NimbleFIX 测试（直接加载 .nfd）
./bench/bench.sh quickfix       # QuickFIX 对比测试
./bench/bench.sh compare        # 完整跨引擎对比
```

上面所有 benchmark 命令都明确使用固定的 QuickFIX 4.4 输入：`bench/vendor/quickfix/spec/FIX44.xml`、`build/bench/quickfix_FIX44.nfd` 或 `build/bench/quickfix_FIX44.nfa`。

完整的 benchmark 方法论、每个指标的起止点、以及标明测试点在流程中位置的图，见 [bench/README.md](bench/README.md)。

---

## 更多文档

- [docs/architecture.md](docs/architecture.md) — 内部架构、模块依赖图、数据流图、线程模型细节
- [docs/development.md](docs/development.md) — 开发指南、测试、`.nfd` 格式规范、性能分析、压力/Fuzz 测试、引擎扩展
- [bench/README.md](bench/README.md) — Benchmark 基础设施、测量边界、完整结果表格
- [docs/public-api.md](docs/public-api.md) — 公共 API 指南、生命周期约定、typed session 边界、最小配置要求
- [docs/design.md](docs/design.md) — 原始设计笔记（中文）

## 项目状态

NimbleFIX 正在积极开发中。核心引擎、会话管理、编解码器和持久化层已实现，并由回归测试覆盖（400+ test case，5k+ assertion）。热路径编码管线持续向设计目标优化中。

## 许可证

详见 [LICENSE](LICENSE)。
