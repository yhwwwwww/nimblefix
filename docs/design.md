# NimbleFIX 设计文档

## 1. 文档目的

本文档定义一个面向低延迟交易场景的 FIX 协议库设计方案。目标是实现一个极速、轻量、可嵌入、可同时作为服务端与客户端使用的 FIX 引擎，并具备完整的 session 管理能力与对 repeating group 的完整支持。

文档重点覆盖以下内容：

- 性能目标与约束
- 总体架构与模块边界
- 字典驱动的消息与 group field 模型
- FIX 版本、方言与校验策略的分层建模
- 完整 session 管理设计
- 服务端与客户端统一内核设计
- 存储、恢复、重发与持久化策略
- 性能优化策略、测试方法与阶段性里程碑

## 2. 设计目标

### 2.1 核心目标

- 提供面向 FIX 4.x / FIXT 的统一架构，工程落地顺序从 FIX 4.4 开始，但整体设计直接覆盖扩展到 4.2/4.3 / FIXT.1.1 的需要。
- 库内热点路径达到亚微秒到个位数微秒级延迟。
- 在 steady-state 下实现零锁、零堆分配、低分支失败率、缓存友好。
- 支持完整 session 管理，包括登录、心跳、超时、序号管理、重发请求、Gap Fill、Sequence Reset、日切与恢复。
- 支持 repeating group 和 nested repeating group。
- 同一套 session core 可同时用于 initiator 和 acceptor。
- 具备可插拔持久化能力，兼顾极低延迟模式与可恢复模式。
- 支持标准字段、用户自定义字段、静态字典字段校验与类型访问。
- 将协议身份拆分为 transport session profile、profile id（含 NormalizedDictionary）与 validation policy 三层。
- 文档定义的是最终目标架构，工程实施允许分步推进，但不引入单独的过渡架构或降级架构。

### 2.2 非目标

- 不以一次性写完所有 FIX 版本的全部语义差异为前提，但架构上保留统一扩展点。
- 核心协议内核不以内建 TLS、Web 管理界面、复杂插件系统为目标。
- 不以内核旁路网络为前提条件。
- 不要求把所有消息都建模成强类型对象树。

## 3. 性能目标与现实边界

### 3.1 分层指标

本项目将性能指标拆为三层，避免混淆库本身成本与系统环境成本。

1. Codec-only

- 单条消息解析与编码延迟目标：数百纳秒到低微秒。
- 输入为内存中连续字节缓冲区。
- 不包括网络、中断、磁盘、跨线程切换。

2. Session loopback

- 包括 session 校验、序号推进、回调投递。
- 使用进程内 loopback 或 mock transport。
- 目标为低微秒级。

3. Wire-to-wire

- 包括 socket、内核网络栈、定时器、持久化与调度干扰。
- 目标视部署环境而定，关注 p50、p99、p999，而非仅看平均值。

### 3.2 性能设计原则

- 每个 session 仅允许单写者线程拥有状态。
- 正常收发路径不进行动态内存分配。
- 热路径不使用哈希表保存字段。
- 避免将协议字段转换为临时字符串对象。
- 将异常路径与恢复路径从 steady-state 路径中隔离。
- 通过预分配 slab、环形缓冲和字典生成代码降低指令与缓存开销。

## 4. 总体架构

系统建议拆分为以下模块：

1. `nimblefix-dictgen`

- 读取 FIX XML 字典。
- 生成字段元数据、消息描述、group 描述、校验规则、类型映射。
- 输出静态代码与紧凑查表结构。

2. `nimblefix-codec`

- 实现字节流解析与编码。
- 输出零拷贝 message view。
- 支持 generic view 与生成的 typed view。

3. `nimblefix-session`

- 实现 session 状态机、序号、重发、恢复、超时、心跳和管理消息处理。
- 管理消息日志与 store 交互。

4. `nimblefix-transport`

- 封装 socket I/O、连接建立、监听、accept 和 reconnect。
- 为 session core 提供统一 transport 接口。

5. `nimblefix-store`

- 持久化消息与 session 元信息。
- 支持 in-memory、mmap append-only、durable batch 模式。

6. `nimblefix-runtime`

- 管理线程模型、时间轮、session 分片、配置、生命周期。

7. `nimblefix-bench`

- 提供 codec benchmark、session benchmark、transport benchmark。

### 4.1 统一分层视图

```text
Application
    |
    v
Typed API / Generic API
    |
    v
Session Engine
    |
    +--> Store
    |
    +--> Timer Wheel
    |
    v
Codec <--> Dictionary Metadata
    |
    v
Transport
```

### 4.2 基本设计判断

- 服务端与客户端共用同一 session engine。
- acceptor/initiator 仅在连接建立、重连策略和监听行为上不同。
- codec 不依赖 session，session 依赖 codec。
- group field 解析完全依赖字典描述，不采用基于猜测的通用树解析。

## 5. 推荐实现语言

建议采用 C++20。

原因如下：

- 更易精确控制对象布局、内存分配与 ABI 成本。
- 更适合构建极薄抽象层，减少不必要的语义负担。
- 更方便引入 SIMD、预取、固定容量容器、自定义 allocator。
- 对于高频交易和网关类场景，C++ 生态更成熟。

如果团队对 Rust 的 zero-copy API、unsafe 边界管理和性能剖析更有经验，Rust 也可以作为替代实现路线。但从性能可控性、对象布局控制和热路径抽象成本看，C++20 更稳妥。

## 6. 字典驱动设计

### 6.1 为什么必须做字典生成

FIX 的性能瓶颈不只在 parse，还在字段语义识别和 group 归属判断。若完全运行时通过 map 或动态表处理：

- tag 语义判断成本高。
- group field 无法低成本正确解析。
- 校验规则会污染热路径。
- 强类型访问会退化为字符串或动态查找。

因此项目必须内置字典生成器，将 FIX XML 编译为紧凑只读元数据。

### 6.2 字典生成结果

字典生成器应至少产出以下结构：

- 字段定义表：tag、名称、基础类型、是否枚举、枚举值表。
- 消息定义表：MsgType、必须字段、允许字段、字段顺序约束。
- Group 定义表：count tag、entry delimiter tag、成员字段集合、嵌套 group 定义。
- 头部与尾部字段规则。
- 管理消息规则：Logon、Heartbeat、TestRequest、ResendRequest、SequenceReset 等。
- 协议 profile 描述：传输层版本、profile id（含 NormalizedDictionary）、校验策略。
- 类型化访问器代码：例如 `NewOrderSingleView`、`ExecutionReportView`。

### 6.3 元数据存储要求

- 所有元数据为编译期或进程初始化期只读结构。
- 按 tag 排序并生成紧凑查找表。
- 热点字段提供直接索引或完美哈希，不走通用 map。
- group 成员关系使用连续数组而非链式结构。

### 6.4 协议建模原则

真实世界里的 FIX 接入，不能把 `BeginString` 当成唯一的”版本开关”。

对内实现必须明确区分以下三层：

1. session transport 语义
2. profile id 与 NormalizedDictionary（最终字典）
3. 当前连接采用的校验严格度

若将这三层混成一个”FIXVersion”枚举，会直接带来以下问题：

- 无法干净支持经典 FIX 4.x 与 FIXT.1.1 的差异。
- 无法表达”FIX 4.4 + 某交易所扩展 + 宽松校验”这种常见现实配置。
- 版本兼容逻辑、session 逻辑和方言兼容逻辑会相互污染。
- typed codegen 会被迫绑死在单一版本枚举上，扩展成本很高。

因此，本项目内部不应把”协议身份”设计成单一字段，而应设计成可组合的 resolved profile。

### 6.5 三层协议模型

建议定义如下三类对象。

1. `TransportSessionProfile`

- 定义 session 层 transport 语义。
- 包括 BeginString 规则、Logon 语义、admin message 处理默认值、头部字段要求、序号恢复能力、FIXT 特殊规则等。
- 这层只关心 session/admin 行为，不关心业务消息字段集合。

2. `NormalizedDictionary`

- 定义当前 profile 使用的最终字典，由 profile id 唯一标识。
- 包括字段、消息、组件、group、枚举和 required 关系。
- 若需要在标准字典上叠加对手方/交易所的扩展字段，由离线字典生成工具（`nimblefix-dictgen`）完成合并，运行时只消费合并后的结果。
- 运行时不区分哪些字段来自标准基线、哪些来自方言扩展——合并后的 `NormalizedDictionary` 是唯一的 truth。

3. `ValidationPolicy`

- 定义当前连接的校验强度。
- 提供 `strict`、`compatible`、`permissive`、`raw-pass-through` 四档。
- 决定未知 tag、字段顺序、重复字段、缺失非关键字段时的行为。

四档行为定义如下：

| 规则 | strict | compatible | permissive | raw-pass-through |
|------|--------|------------|------------|------------------|
| 校验 CompID | 是 | 是 | 否 | 否 |
| 要求 Logon 含 DefaultApplVerID | 是 | 否 | 否 | 否 |
| 要求 PossDup 含 OrigSendingTime | 是 | 否 | 否 | 否 |
| 拒绝过期 MsgSeqNum | 是 | 是 | 否 | 否 |
| 要求已知 MsgType | 是 | 否 | 否 | 否 |
| 校验 required 字段 | 是 | 是 | 否 | 否 |
| 拒绝未知字段 | 是 | 否 | 否 | 否 |
| 拒绝重复字段 | 是 | 否 | 否 | 否 |
| 拒绝非法 group 结构 | 是 | 是 | 否 | 否 |
| 解析消息体 | 是 | 是 | 是 | **否** |

`raw-pass-through` 面向消息转发场景。收发两侧的工作模式如下：

**接收侧**：只提取消息边界和 session 层关键字段（`BeginString`、`BodyLength`、`MsgType`、`MsgSeqNum`、`SenderCompID`、`TargetCompID`、`SendingTime`、`CheckSum`），不解析消息体的应用字段，也不做任何内容校验。解码输出应包含：

- session 层字段的解析值
- 应用消息体的原始字节区间（body 起止偏移），供转发侧直接引用

**发送侧**：提供 header rewrite + raw body passthrough 的编码路径。转发时只需：

1. 用下游 session 的 `SenderCompID`、`TargetCompID`、`MsgSeqNum`、`SendingTime` 重写 header。
2. 可选插入路由字段（`OnBehalfOfCompID(115)`、`DeliverToCompID(128)` 等）。
3. 拼接原始 body 字节（零拷贝引用接收缓冲区）。
4. 重算 `BodyLength` 和 `CheckSum`。

该路径不依赖字典、不遍历字段、不构造消息对象，延迟开销仅为 header 序列化 + checksum 计算。

这三层在会话建立前合成为不可变的 `ResolvedProtocolProfile`，供 codec、session 和 typed API 共同使用。

### 6.6 经典 FIX 4.x 与 FIXT.1.1 的统一建模

经典 FIX 4.x 与 FIXT.1.1 最大的架构差异，不在字段本身，而在 transport version 与 application version 是否解耦。

建议统一按以下方式建模：

1. 对经典 FIX 4.x

- `BeginString` 同时决定 transport 语义和默认字典基线。
- 例如 `FIX.4.2` 可映射为：
    - `TransportSessionProfile = FIX42_CLASSIC`
    - `NormalizedDictionary = FIX42_STD`（由 profile id 标识）

2. 对 FIXT.1.1

- `BeginString=FIXT.1.1` 只决定 transport 语义。
- 应用消息版本由 `DefaultApplVerID` 或消息级 `ApplVerID` 决定。
- 因此一个 session transport profile 可以绑定多个 NormalizedDictionary。

统一采用这套内部模型后，代码层面无需把 FIXT 当成特例分支，而是按相同 profile 解析流程工作。

### 6.7 字典合并规则（仅限离线工具）

字典合并仅发生在离线字典生成工具（`nimblefix-dictgen`）中，运行时不存在合并逻辑。

工具接受一个基线 `.ffd` 文件和零个或多个增量 `.ffd` 文件，按以下规则合并为单一 `NormalizedDictionary`：

1. 字段级合并

- 允许新增私有 tag。
- 允许扩展标准字段的枚举值集合。
- 允许添加别名、显示名和 venue 说明。
- 修改基础类型属于破坏性变更，默认禁止，必须显式开启强制覆盖。

2. 消息级合并

- 允许新增私有 `MsgType`。
- 允许在标准消息上增加可选字段或 venue 必填字段。
- 允许按 venue 规则放宽或收紧 required 约束。

3. Group 级合并

- 允许扩展标准 group 的成员字段。
- 允许新增 nested group。
- 修改 delimiter 属于结构性变更，默认要求显式确认。

4. Header / Trailer 合并

- 允许附加 venue 自定义头字段。
- 不允许悄然改变关键 session tag 的核心语义。

合并输出为 `NormalizedDictionary`，其数据布局与直接编写的单文件字典完全一致。运行时只消费最终产物，不感知合并过程。

### 6.8 Resolved Protocol Profile

建议在运行时维护一个不可变 profile 对象：

```text
ResolvedProtocolProfile {
        transport_profile_id;
        profile_id;
        validation_policy;
        normalized_dictionary_ref;
        admin_rules_ref;
        typed_view_table_ref;
}
```

设计要求：

- profile 一旦绑定到 session，不在会话中途变化。
- codec 使用它解释消息结构和 group。
- session engine 使用它解释 admin 语义和校验策略。
- typed API 使用它定位生成访问器与 message schema。

### 6.9 建连时的 profile 绑定

profile 绑定必须发生在 session 进入 `Active` 之前。

1. Initiator

- profile 由本地配置预先指定。
- connect 和 Logon 过程只负责验证远端是否符合预期。

2. Acceptor

- 初始可先按监听器或地址范围缩小候选集合。
- 收到 Logon 后，根据 `BeginString`、CompID、可选认证字段、`DefaultApplVerID` 等选择最终 profile。
- 一旦选定，后续解析、校验、重发与 typed access 都绑定到该 profile。

这意味着 acceptor 不能只靠 `BeginString` 路由，而要路由到 `CounterpartyProfile`。

### 6.10 对代码生成与 API 的影响

多版本与方言支持的难点，不在 parser 本身，而在 codegen 和校验层。

建议采用以下策略：

- typed view 和 typed builder 按 `NormalizedDictionary` 生成，不区分字段来源。
- generic API 始终可用，用于低频或调试场景。
- session 代码依赖 `TransportSessionProfile` 与 `ValidationPolicy`，而不是依赖单一 `FIXVersion` 枚举。

这样可以让”支持新版本”和”支持新对手方”成为加 profile 的动作，而不是改 parser 核心。

### 6.11 字典生成与 profile 产物流水线

对于 C++ 实现，字典系统建议采用”离线生成 + 启动期装载”的流水线，而不是在进程热路径内解析 XML。

推荐流水线如下：

1. 源定义层

- 标准 FIX XML 或 `.ffd` 文件作为基线输入。
- venue / broker / counterparty 的增量 `.ffd` 文件通过 `--merge` 合并。
- validation policy 与 profile 绑定规则作为独立运行时配置。

2. 生成层（离线工具 `nimblefix-dictgen`）

- 合并所有输入为单一 `NormalizedDictionary`。
- 为热点消息生成 typed view、typed builder、template encoder，并生成可由 `FixedLayout` / hybrid encoder 直接消费的字典元数据。
- 为 generic fallback 生成紧凑元数据、查找表和规则表。
- 生成一个或多个可装载的 profile artifact。

3. 启动层

- 进程启动时加载 artifact（或直接加载 `.ffd` 文件）。
- 绑定到 profile registry。
- session 建立时只引用已经装载好的 profile。

steady-state 路径不再解析 XML，不再合并字典，也不再临时构造大对象图。

### 6.12 Artifact 概念与格式要求

这里的 `artifact` 指“预编译的协议 profile 产物”，而不是 XML，也不是把进程内 C++ 对象直接序列化到磁盘。

一个 artifact 最好对应一个可直接装载的 `ResolvedProtocolProfile` 静态输入，至少包含：

- profile header
- string table
- field definitions
- message definitions
- group definitions
- admin rules
- validation rules
- lookup tables
- template descriptors

artifact 文件格式建议满足以下约束：

- 只使用固定宽度整数、字符串表、连续数组和相对偏移。
- 不存裸指针、不依赖编译器 padding、不依赖宿主 ABI。
- 带有 `magic`、format version、schema hash、profile id。
- 支持只读映射和零拷贝视图封装。

建议格式示意：

```text
ProfileArtifactHeader
SectionTable[]
StringTable
FieldDefs
MessageDefs
GroupDefs
AdminRules
ValidationRules
LookupTables
TemplateDescriptors
```

### 6.13 生成代码与 artifact 的分工

推荐采用“生成代码 + 外部 artifact”的混合模式，而不是二选一。

1. 生成代码负责：

- typed view
- typed builder
- hot message template encoder
- profile-specific 的 fixed-layout / hybrid writer 封装
- profile-specific 的内联访问器和常量

2. Artifact 负责：

- `NormalizedDictionary`
- group / message / field 元数据
- lookup table
- validation / admin rule 表
- generic fallback 所需结构

这样可以同时满足两个目标：

- hot path 的关键逻辑直接编进程序，获得最薄调用链。
- 规则与元数据保持外部化，便于增量装载和 profile 管理。

若部署场景中的 profile 完全固定，也可以把 artifact 生成为 `constexpr` 只读数组直接编进二进制，但运行时视图模型应保持一致。

### 6.14 启动期加载流程

进程启动时建议按以下顺序装载 profile artifact：

1. 读取 engine 配置与 profile 列表。
2. 以只读方式打开 artifact 文件。
3. 优先使用 `mmap` 将 artifact 映射到内存。
4. 校验 header、format version、schema hash、profile id、文件长度与 section 边界。
5. 将各 section 封装为只读 view，而不是重新拷贝成堆对象。
6. 构造轻量 `LoadedProfile` / `ResolvedProtocolProfileView`。
7. 将 profile 注册到 registry。
8. 对热点 profile 可选执行页面预热、`madvise` 或 `mlock`。

session 建立时只保存对已装载 profile 的只读引用。steady-state 下不再触碰 XML、字典合并器或大规模初始化逻辑。

生成代码与 artifact 之间必须携带同一个 `schema_hash`，启动时强制校验一致性，避免代码与元数据错配。

### 6.15 发送路径策略

真实世界里高频路径上最常见的不是”纯标准 FIX 消息”，而是”某个 profile 下叠加稳定 custom 字段”的消息模板。

因此 hot path 的建模单位不应是”FIX 4.4 消息”，而应是”某个 profile 下的消息模板”。

发送时所有需要发送的字段原则上都应在 `NormalizedDictionary` 中声明。发送路径分为四层，其中前三层是真正的编码路径，第四层是 passthrough 转发路径：

1. Fixed layout path：dictionary-driven `FixedLayout` + fixed encoder

- 面向字段集合和顺序长期稳定的高频业务消息。
- `FixedLayout` 由已加载的 `NormalizedDictionary` / profile metadata 构建，预先完成 tag -> slot 的 O(1) 映射。
- 编码使用预编译模板或固定 writer，仅回填变动字段、`BodyLength`、`CheckSum`。

2. Fragment append path：caller-trusted prebuilt header/body fragments

- 面向“主干字段稳定，额外 header/body 字节已经在调用方侧准备好”的生产路径。
- encoder 入口只接受已经序列化好的 `tag=value<SOH>` 片段，例如 `EncodedOutboundExtrasView{header_fragment, body_fragment}`。
- `header_fragment` 只允许追加在 managed session header 之后、业务 body 之前；`body_fragment` 只允许追加在业务 body 尾部、`CheckSum` 之前。
- 该路径不负责 mixed extras 的分类、冲突消解或字典校验；它的目标是把这部分工作移出发送热路径。
- FIX 结构顺序仍必须由调用方保证：`8/9` 在最前、`10` 在最后，repeating group 仍需按规范编码。
- 这是一条 caller-trusted 的窄接口，而不是面向任意运行时字段切片的通用混合层。

3. Generic path：`MessageBuilder` + generic encoder

- 面向低频、调试或完全动态场景。
- 按 tag 逐字段写入，不依赖预编译 fixed layout。
- 可按需要接入字典校验，也可在宽松策略下直接编码。

4. Forwarding path：header rewrite + raw body passthrough

- 面向 `raw-pass-through` 模式下的消息转发。
- 不解析也不重建消息体，直接引用接收缓冲区的原始 body 字节。
- 仅重写 session header（`SenderCompID`、`TargetCompID`、`MsgSeqNum`、`SendingTime`），可选插入路由字段（`OnBehalfOfCompID`、`DeliverToCompID`）。
- 重算 `BodyLength` 和 `CheckSum`。
- 不依赖字典，延迟开销仅为 header 序列化 + checksum。

因此，这里的约束不是“完全禁止 hot path 与 generic 之间的中间态”，而是“不鼓励无约束的 ad-hoc extra setter”。正式支持的生产发送构造方式是 fixed、fragment、generic，再加一个独立的 forwarding 转发路径。

## 7. 编解码设计

### 7.1 解析目标

解析器输出的不是“拥有数据”的消息对象，而是对原始 buffer 的轻量视图。

每条消息的最小表示建议包含：

- 原始字节区间
- 字段槽数组 `FieldSlot[]`
- 已识别消息类型
- 已识别关键 session 字段的快速索引
- group frame 索引区
- 校验状态位

### 7.2 字段槽表示

字段槽建议采用固定小结构：

```text
FieldSlot {
    uint32_t tag;
    uint32_t value_offset;
    uint16_t value_length;
    uint16_t flags;
}
```

说明：

- `value_offset` 和 `value_length` 指向原始消息 buffer。
- 不在热路径构造 `std::string`。
- `flags` 可表示是否在 header/trailer/group 中、是否重复、是否通过类型预解析。

### 7.3 解析流程

热路径可按如下顺序执行：

1. 扫描 `8=` 与 `9=`。
2. 根据 `BodyLength` 定位消息边界。
3. 遍历正文字段，逐个提取 `tag=value<SOH>`。
4. 对关键 tag 执行快速分派：`35`、`34`、`49`、`56`、`52`、`10`、group count tag。
5. 填充字段槽数组与关键字段缓存。
6. 执行 checksum 校验。
7. 输出 message view。

### 7.4 编码流程

编码器需支持三种编码模式，并保留一个独立的 forwarding 重写模式：

1. Fixed layout encode

- 面向 dictionary-driven `FixedLayout`。
- 预先确定固定片段、slot 映射和回填位置。

2. Fragment append encode

- 在 fixed/template/generic encode 的主干输出前后拼接 caller-trusted 的 `header_fragment` / `body_fragment`。
- fragment 本身已经是最终字节表示，encoder 只负责把它们放到正确的 header/body 边界位置。
- 该模式不承担 mixed field slice 的预分类、顺序归并或 validation policy 判定。

3. Generic encode

- 面向灵活性。
- 支持按 tag 写入字段。

`raw-pass-through` 的 header rewrite 继续作为独立 forwarding path，不与上述三种消息构造路径混淆。

编码器要求：

- 不做临时字符串拼接。
- 允许写入预分配 buffer。
- 对整数、价格、时间戳提供无分配格式化器。
- 将 `BodyLength` 和 `CheckSum` 放到单独回填步骤。
- fixed layout 的布局确认发生在 profile 加载后，通过字典派生的 `FixedLayout` 完成，而不是每条消息重复构建。
- fragment extra bytes 必须已经是合法的 `tag=value<SOH>` 序列，并由调用方保证它们出现在正确的 header/body 边界。
- 不要求 body 字段按 tag 数值排序，但必须满足 FIX 的 header/trailer/group 结构顺序，以及当前 profile 对已知字段定义的顺序约束。
- 若需要对外部字段做分类、去重或字典校验，应在调用 encoder 之前完成，而不是把这些判断留在逐字节编码循环里。

### 7.5 校验分层

校验要分层，以免拖慢热路径：

- 第 0 层：边界、tag 语法、checksum。
- 第 1 层：关键 session 字段完整性。
- 第 2 层：消息级字段合法性。
- 第 3 层：业务语义校验。

默认热路径只强制第 0 层和第 1 层。第 2 层和第 3 层可以根据配置或环境选择开启。

## 8. 消息模型设计

### 8.1 双轨模型

库暴露两套访问方式：

1. Generic View

- 适合处理未知消息、自定义字段、透传场景。
- 提供 `find(tag)`、遍历字段、获取 group entry 视图。

2. Typed View

- 由字典生成器生成。
- 提供 `msg.cl_ord_id()`、`msg.order_qty()` 之类的类型化访问。

双轨并存的原因：

- 全强类型会降低灵活性。
- 全动态会失去性能和校验优势。
- FIX 私有扩展非常常见，必须保留 generic 通道。

### 8.2 快速字段缓存

对以下字段必须做快速缓存：

- `8` BeginString
- `9` BodyLength
- `35` MsgType
- `34` MsgSeqNum
- `49` SenderCompID
- `56` TargetCompID
- `52` SendingTime
- `10` CheckSum

这些字段在 session 管理中访问频率极高，不应每次通过通用查找获取。

## 9. Repeating Group 设计

### 9.1 设计目标

- 正确支持标准 group 与 nested group。
- 解析时不构造树状堆对象。
- group entry 访问保持连续内存与视图语义。
- 利用字典生成的 delimiter 规则判断 entry 边界。

### 9.2 核心元数据

每个 group 定义至少包括：

- `count_tag`
- `delimiter_tag`
- `member_tags[]`
- `nested_groups[]`
- `required_member_bitmap`

### 9.3 运行时表示

建议使用 group frame 与 entry 索引的方式表示：

```text
GroupFrame {
    uint32_t def_id;
    uint32_t parent_frame;
    uint32_t entry_start_index;
    uint16_t entry_count;
    uint16_t depth;
}

GroupEntryIndex {
    uint32_t first_field_slot;
    uint16_t field_count;
    uint16_t nested_frame_begin;
}
```

说明：

- 一个消息内的所有 group 元数据都存在连续数组中。
- entry 本身引用已有字段槽，不复制字段值。
- nested group 通过 frame 区间引用，而不是堆指针。

### 9.4 解析算法概要

group 解析不依赖消息文本缩进或显式结束符，而依赖 FIX 字典定义。

算法建议：

1. 当遇到 group count tag 时，从字典取得 group 定义。
2. 创建一个活动 frame 并读取 entry 计数。
3. 后续字段遇到 delimiter tag 时开启新 entry。
4. 字段若属于当前 group 成员，则并入当前 entry。
5. 字段若属于 nested group count tag，则递归进入嵌套 frame，但底层实现使用显式栈，避免函数递归。
6. 字段不再属于当前 frame 时，关闭当前 group 或上浮到父 frame。

### 9.5 API 示例

```text
auto parties = msg.group(NoPartyIDs);
for (auto entry : parties) {
    auto party_id = entry.get_string(PartyID);
    auto party_role = entry.get_int(PartyRole);
}
```

Typed API 可在此基础上包装成更具体的类型化 entry view。

## 10. Session 管理设计

### 10.1 Session 范围

一个 session 由以下键识别：

- Transport BeginString 或 transport profile
- SenderCompID
- TargetCompID
- 可选 LocationID
- 可选 SessionQualifier

`SenderSubID(50)` / `TargetSubID(57)` 不属于 `SessionKey`。它们是每消息可选的 session envelope 参数：调用方提供则写入 header，未提供则省略，同一 session 可以发送不同的 `50/57`。

内部建议以规范化后的 `SessionKey` 做唯一键。

需要注意的是，`SessionKey` 与 `ResolvedProtocolProfile` 不是同一个概念。

- `SessionKey` 用于唯一标识传输会话。
- `ResolvedProtocolProfile` 用于描述该会话绑定的字典与校验策略。

在经典 FIX 4.x 中，两者通常高度相关；在 FIXT.1.1 或同一对手方存在多个业务字典时，两者必须显式分离。

这里的 `session` 指逻辑 FIX Session，而不是当前某一次 TCP 连接。

建议在运行时显式区分以下对象：

- `PendingConnection`
- `TransportConnection`
- `FixSession`

它们的职责分别如下：

- `PendingConnection` 表示刚建立但尚未完成 Logon、认证和 profile 绑定的接入连接。
- `TransportConnection` 表示当前活跃的 socket、读写缓冲和 poller 注册关系。
- `FixSession` 表示长期存在的逻辑会话，持有序号、心跳、超时、重发、store、profile 和业务回调绑定等状态。

一个 `FixSession` 可以跨多次断线重连持续存在，而 `TransportConnection` 只是它在某个时刻附着的 transport。对于 acceptor 来说，只有在 Logon 校验、认证与 profile 选择完成后，连接才应正式绑定到某个 `FixSession`。

### 10.2 Session 状态

建议最少定义以下状态：

- `Disconnected`
- `Connecting`
- `Connected`
- `PendingLogon`
- `Active`
- `AwaitingLogout`
- `ResendProcessing`
- `Recovering`
- `Closed`

状态转移由 transport 事件、管理消息和定时器驱动。

### 10.3 正常路径

正常路径核心流程如下：

1. 建立连接。
2. 发送或接收 Logon。
3. 校验身份、心跳间隔、序号策略。
4. 切换到 `Active`。
5. 正常收发应用消息与管理消息。
6. 周期性发送 Heartbeat，按需要发送 TestRequest。
7. 收到 Logout 或出现超时则关闭。

### 10.4 序号管理

每个 session 维护：

- 下一个发送序号 `next_out_seq`
- 下一个预期接收序号 `next_in_seq`
- 重发区间与 gap 集
- possdup / possresend 状态

设计原则：

- 序号推进由 session owner 线程独占。
- 收到乱序消息时不立刻破坏 steady-state 结构。
- 重发请求逻辑与正常消息分发解耦。

### 10.5 管理消息处理要求

必须原生支持：

- Logon
- Logout
- Heartbeat
- TestRequest
- ResendRequest
- SequenceReset
- Reject

处理要求：

- 正确识别 `GapFillFlag`。
- 正确处理 `ResetSeqNumFlag`。
- 支持 PossDup/PossResend。
- 支持发送端重放消息与 gap fill 混合返回。
- 对非法序号、错误 CompID、校验失败执行可配置策略。

### 10.6 超时与心跳

不建议每个 session 配置一个独立系统定时器。建议使用时间轮或分层时间轮。

每个 session 至少维护：

- 上次接收时间
- 上次发送时间
- 心跳间隔
- TestRequest 截止时间
- Logout 截止时间

时间轮优势：

- 减少系统调用与定时器对象数量。
- 容易批量推进大量 session。
- 对 acceptor 场景更友好。

## 11. 重发与恢复设计

### 11.1 重发处理原则

- 收到缺口时生成 `ResendRequest`。
- store 返回消息时，按 FIX 规则判断哪些可重放，哪些必须 gap fill。
- 重发时保留必要头字段并设置 PossDup。
- 支持区间压缩，避免过多碎片化 gap 请求。

### 11.2 恢复模式

建议至少支持三种恢复模式：

1. 热重启恢复

- 从本地 store 恢复序号与最近消息索引。
- 尽量不依赖对端重新建立全部状态。

2. 冷启动恢复

- 仅恢复 session 元信息。
- 不恢复完整消息日志。

3. 无恢复低延迟模式

- 仅内存 store。
- 适合测试、撮合前置、内网短生命服务。

### 11.3 日切策略

日切必须做成显式策略，而不是散落在 session 逻辑中。

建议支持：

- 固定本地时间日切
- UTC 日切
- 外部控制日切
- 不自动重置序号

## 12. Store 设计

### 12.1 抽象接口

store 最少需要支持：

- 保存发送消息
- 保存接收消息元信息
- 查询指定序号区间的历史消息
- 持久化/恢复 session 序号
- 截断或归档历史数据

### 12.2 存储模式

1. In-memory store

- 极低延迟。
- 无持久化保障。
- 适合 benchmark、测试、短生命周期服务。

2. Mmap append-only store

- 在性能与恢复间折中。
- 通过顺序追加减少写放大。

3. Durable batch store

- 周期性 flush 或按策略同步。
- 适合需要更高可靠性的 session。

### 12.3 数据布局建议

消息日志可采用追加式结构：

```text
RecordHeader {
    uint32_t seq_num;
    uint32_t msg_len;
    uint64_t timestamp_ns;
    uint16_t flags;
    uint16_t reserved;
}
payload...
```

配合独立索引区，可快速按序号定位历史消息。

## 13. 服务端与客户端统一设计

### 13.1 统一内核

同一 session engine 必须可同时支撑：

- Initiator
- Acceptor

差异下沉到 transport 与会话建模层：

- Initiator 负责主动连接、重连退避、登录发起。
- Acceptor 负责监听、accept、新连接到 session 的绑定策略。

### 13.2 Acceptor 设计要求

- 支持单监听端口承载多个 session。
- 支持在 Logon 成功前以 `PendingConnection` 管理新接入 socket，而不是过早创建正式 session。
- 支持连接建立后按 Logon 中的 CompID 路由到 session。
- 支持在 Logon 后将连接绑定到 `CounterpartyProfile`，并据此选择 transport profile 与 NormalizedDictionary。
- 支持未知 session 的拒绝策略。
- 支持动态 session 工厂或预配置 session 白名单。

### 13.3 Initiator 设计要求

- 支持断线重连与退避策略。
- 支持启动时自动登录。
- 支持连接建立前预构造 Logon 模板。

## 14. 线程与并发模型

### 14.1 核心原则

- 单个 session 的协议状态永远由单个线程独占。
- 多个 session 可分片到多个 event loop / worker。
- 应用线程与 session 线程之间通过无锁或单生产者单消费者队列通信。

### 14.2 推荐模型

建议支持以下两种运行模式：

1. Embedded mode

- 适合单连接低延迟场景。
- 应用与 session 在同一线程。
- 业务回调直接运行在 session owner 线程上。

2. Sharded runtime mode

- 适合多 session 服务端。
- session 按 `SessionKey` 哈希到固定 worker。

对于 acceptor，默认建议采用 `Sharded runtime mode`，而不是共享线程池随机处理连接和消息。

### 14.3 Acceptor 推荐线程拓扑

建议 acceptor 采用三层结构：

1. Front-door thread

- 负责 `accept()` 新连接。
- 负责读取最小必要的 Logon 数据。
- 负责基础认证、选择 `CounterpartyProfile`、决定目标 worker。
- 只做一次性 handoff，不长期持有已绑定 session 的协议状态。

2. Session worker shards

- 每个 worker 持有自己的 poller、timer wheel、session 表、codec 实例和 outbound 队列。
- 每个 `FixSession` 永远只属于一个 worker。
- 该 worker 独占执行该 session 的解析、序号推进、心跳、重发、编码和 socket 写出。

3. Optional app shards

- 在库与应用解耦模式下，可为每个 worker 配对一个应用分片线程。
- FIX worker 与 app shard 之间优先使用单生产者单消费者队列通信。

该模型的关键不是“多线程”，而是“单 session 单写者”。同一个 `FixSession` 的协议状态不能被多个线程并发修改。

### 14.4 Acceptor 连接接入流程

推荐流程如下：

1. Front-door thread `accept()` 新连接。
2. 连接进入 `PendingConnection` 状态。
3. 读取并解析最小必要的 Logon 字段。
4. 根据 `BeginString`、CompID、可选认证字段、`DefaultApplVerID` 等选出目标 `CounterpartyProfile`。
5. 根据 `SessionKey` 或 `CounterpartyProfile` 计算目标 worker。
6. 将 socket、已读取 buffer 和初始解析结果一次性 handoff 给目标 worker。
7. 目标 worker 完成 `FixSession` 绑定并推进到 `PendingLogon` 或 `Active`。

这里允许一次建连阶段的跨线程迁移，但不允许一个已绑定 session 在 steady-state 下反复跨线程漂移。

### 14.5 FIX 库与应用的线程关系

库与应用之间建议显式支持两种关系模型：

1. Inline mode

- 应用回调直接运行在 session owner 线程上。
- 优点是路径最短、零拷贝最自然、回包最及时。
- 约束是业务代码不能阻塞、不能做磁盘 I/O、不能等待共享锁、不能把 `MessageView` 隐式带到其他线程继续使用。

2. Queue-decoupled mode

- FIX worker 负责收包、parse、session 校验、序号、编码和 socket 写出。
- 应用线程负责业务处理。
- 双方通过 inbound / outbound 队列通信。

在 `Queue-decoupled mode` 下，应用线程不应直接修改 session 状态，也不应直接决定最终发送序号。应用只能提交“发送意图”，真正的头字段补全、序号分配、store 记录和写 socket 必须回到 session owner 线程执行。

对于低延迟多 session 服务端，推荐采用“同分片配对”关系：`worker_i` 负责一组 FIX session，`app_i` 负责同一组 session 的业务处理，二者之间使用定向队列而不是通用线程池。

### 14.6 锁使用原则

- 热路径不允许使用互斥锁。
- 配置加载、session 注册、统计汇总等冷路径可以使用锁。
- 指标上报不得反向污染协议线程。

## 15. 内存管理设计

### 15.1 热路径内存原则

- 所有 message buffer 来自预分配池或固定容量 slab。
- 解析结果数组使用固定容量或对象池。
- group frame 与 entry 索引复用预分配空间。
- 禁止在 steady-state 路径中创建临时 `std::string`、`std::vector`。

### 15.2 Buffer 生命周期

接收消息推荐生命周期：

1. transport 将字节写入接收 ring/slab。
2. codec 在原 buffer 上解析。
3. session 完成处理前，message view 持有 buffer 引用。
4. 应用若需要异步保留消息，必须显式复制。

这样可以保证默认路径零拷贝，同时明确 ownership 边界。

## 16. API 设计草案

### 16.1 应用侧抽象

建议暴露以下高层对象：

- `Engine`
- `Session`
- `SessionHandle`
- `Acceptor`
- `Initiator`
- `ProtocolProfile`
- `CounterpartyProfile`
- `MessageView`
- `GroupView`
- `Store`

### 16.2 事件回调

应用可接收三类事件：

- `on_session_event`
- `on_admin_message`
- `on_app_message`

理由：

- 管理消息与业务消息在处理优先级和失败策略上不同。
- session 状态变化需要与消息回调解耦。

### 16.3 发送接口

建议同时支持：

- 发送 typed / fixed-layout builder
- 发送 builder + caller-trusted encoded fragments
- 发送 generic field writer
- 发送原始 FIX buffer（受限模式）

原始 buffer 模式适合桥接已有系统，但默认不应绕过 session 头字段和序号管理。

### 16.4 线程亲和接口约束

建议把 API 显式分成线程亲和对象和线程安全句柄两类：

- `Session` 或 `SessionCore` 只允许在 owning worker 线程内使用。
- `SessionHandle` 允许被应用线程持有，但仅暴露入队发送、查询快照、订阅事件等线程安全操作。
- `MessageView` 默认是回调作用域内、线程亲和的零拷贝视图；若应用要跨线程保留数据，必须显式复制。

这层边界必须在 API 设计时就定死，否则后续很容易把 session 状态暴露给多个线程，破坏单写者模型。

## 17. 错误处理策略

错误需按严重程度分层：

- 致命协议错误：关闭 session。
- 可恢复协议错误：发送 Reject 或进入恢复流程。
- 配置错误：启动失败。
- 内部资源错误：根据模式选择背压、丢弃或关闭。

对外 API 不建议在热路径大量抛异常。内部实现宜使用状态码或 `expected` 风格结果，并在边界层转换成更友好的错误模型。

## 18. 可观测性设计

### 18.1 指标

建议暴露：

- 每 session 收发消息数
- 管理消息数量
- 重发请求次数
- gap fill 次数
- parse 失败数
- checksum 失败数
- 当前待发队列深度
- store flush 延迟

### 18.2 Trace 与调试

必须区分生产与调试模式：

- 生产模式默认关闭逐消息日志。
- 调试模式可记录原始 FIX 流与 session 事件。
- trace 记录应写入独立缓冲，不阻塞热路径。

## 19. 性能优化重点

### 19.1 解析热点优化

- 对 tag 解析使用整数累加而非字符串转换。
- 尽量减少分支链长度。
- 对关键字段使用专门快速路径。
- 在可能情况下利用向量化扫描 `SOH` 与 `=`。

### 19.2 编码热点优化

- 预拼接固定字段片段。
- 整数和十进制格式化使用定制实现。
- `BodyLength`、`CheckSum` 回填最小化扫描范围。

### 19.3 运行时优化

- 支持 CPU pinning。
- 支持 busy poll 与 blocking I/O 两种模式。
- 支持 socket 选项优化，如 `TCP_NODELAY`。
- 支持预热对象池与消息模板。

## 20. 测试策略

### 20.1 单元测试

- 字段解析与编码正确性
- checksum 与 BodyLength 校验
- group 解析覆盖标准与嵌套场景
- session 状态转移
- ResendRequest / SequenceReset 处理

### 20.2 兼容性测试

- 与现有 FIX 引擎互联测试
- 标准管理消息往返测试
- 不同 FIX 版本字典校验测试

### 20.3 负向测试

- 非法字段顺序
- 缺失必填字段
- 错误 checksum
- 错误 BodyLength
- 错误 group delimiter
- 错误 MsgSeqNum 与 PossDup 组合

### 20.4 Fuzz 测试

codec 和 group parser 非常适合 fuzz。必须对 parse 输入做长期 fuzz，以尽早发现越界、死循环、非法状态转移和异常分支膨胀问题。

## 21. Benchmark 方案

### 21.1 Benchmark 分类

1. Parse benchmark

- 单条不同消息类型解析耗时。

2. Encode benchmark

- 单条不同消息类型编码耗时。

3. Session benchmark

- 包括序号推进、心跳判断、管理消息处理。

4. Replay benchmark

- 指定区间重发吞吐与尾延迟。

5. End-to-end benchmark

- 基于 TCP loopback 与双机场景。

### 21.2 指标要求

所有 benchmark 至少输出：

- p50
- p95
- p99
- p999
- 吞吐量
- 分配次数
- CPU 利用率
- cache miss 与 branch miss（若可获取）

## 22. 配置设计

配置建议分为四层：

1. Engine 级配置

- worker 数量
- timer wheel 参数
- 内存池容量
- trace 开关

2. Transport 级配置

- 主机与端口
- connect 超时
- reconnect 退避
- socket 选项

3. Protocol 级配置

- transport profile ID
- profile ID（对应 NormalizedDictionary）
- validation policy
- unknown tag / group 的处理策略
- 是否启用严格字段顺序与重复字段检查

4. Session / Counterparty 级配置

- BeginString
- CompID 信息
- 心跳间隔
- 日切策略
- 重发策略
- store 类型
- profile 选择规则

## 23. 代码组织建议

建议目录结构如下：

```text
nimblefix/
  include/
  src/
    dict/
    codec/
    session/
    transport/
    store/
    runtime/
  tools/
    dictgen/
  tests/
  bench/
  docs/
```

对于库使用者，头文件层应保持薄且稳定。生成代码与手写核心逻辑要隔离，便于升级字典与维护 ABI。

## 24. 工程实施顺序

以下顺序仅表示工程落地顺序，不代表会维护多套架构目标。本文档定义的整体架构从一开始就是固定目标。

### 步骤 1: 协议 profile、字典与 codec 内核

- 实现 FIX XML 解析与字典生成器。
- 实现 artifact 文件格式与 profile loader。
- 实现基础 parser 与 encoder。
- 支持 generic message view。
- 支持 repeating group 基础解析。
- 建立 codec benchmark。

### 步骤 2: Session core

- 实现 initiator/acceptor 共用的 session 状态机。
- 支持 Logon/Logout/Heartbeat/TestRequest。
- 支持序号推进与基础错误处理。
- 接入 in-memory store。

### 步骤 3: 重发与恢复

- 实现 ResendRequest、SequenceReset、Gap Fill。
- 接入 mmap store。
- 支持热重启恢复。

### 步骤 4: Typed API 与性能打磨

- 生成 typed view 与 typed builder。
- 完成模板化编码。
- 做 CPU、分支、缓存层面的性能剖析和优化。

### 步骤 5: 生产环境增强

- 完成配置系统、指标系统、调试 trace。
- 完成互联测试、fuzz、长稳测试。
- 评估 io_uring 或更激进 I/O 选项。

## 25. 关键风险与应对

### 风险 1: group 解析复杂度被低估

应对：

- 强制字典生成驱动。
- 尽早建立 nested group 测试矩阵。

### 风险 2: 恢复逻辑污染热路径

应对：

- 明确正常路径与恢复路径分层。
- 单独设计 replay 子系统与 store 接口。

### 风险 3: 过早抽象导致性能退化

应对：

- 实现时以数据布局和热路径为中心。
- 避免过度泛化容器和接口层。

### 风险 4: 只看平均延迟导致误判

应对：

- benchmark 强制输出尾延迟。
- 对不同模式分别建基线。

## 26. 架构结论

该项目的可行路径不是先做一个“功能齐全的通用 FIX 框架”，而是先做一个以字典驱动、零拷贝视图、单写者 session core 为中心的低延迟协议引擎。

最重要的三个技术支点是：

1. 字典生成器
2. 零拷贝 codec
3. 与热路径解耦的完整 session 管理

只要这三个基础结构设计正确，后续扩展 FIX 版本、增加 typed API、完善恢复能力和接入更激进的网络模式都会比较顺畅。反之，如果一开始用动态 map、对象树和混合状态逻辑来实现，后面几乎一定会在 group 解析、重发恢复和尾延迟上付出很大代价。

## 27. 建议的下一步

下一步建议直接进入以下工作项：

1. 先定义 `TransportSessionProfile`、`NormalizedDictionary`、`ValidationPolicy` 的数据模型。
2. 再定义字典生成器输出格式与 artifact 布局。
3. 再实现最小 parser/encoder 原型，并用几类典型消息建立第一批 benchmark 基线。

在工程顺序上，先把 protocol profile 和字典模型定准，再做 codec，会比先铺 transport 或大而全 API 更稳。
