# NimbleFIX Generated-First API 重构设计稿

> Status: archived redesign draft. This document records the motivation and
> target shape for the generated-first API work. It intentionally contains
> historical "current state" notes and line references from the time it was
> written; do not treat it as the authoritative description of today's public
> API. Use `README.md` and `docs/public-api.md` for current behavior.

本文档定义 NimbleFIX 下一代公共 API 的目标形态。设计基线为：

- 不考虑兼容
- `dictgen` 生成物是唯一主业务消息 API
- runtime 对外以 typed session / typed callbacks / profile bind 为中心
- tag、layout、buffer、encoded body、borrowed/owned 细节全部下沉到 internal 或 advanced

The original execution checklist was tracked outside the checked-in documentation.

---

## 1. 设计目标

### 1.1 目标

1. 让应用层围绕“业务消息”编程，而不是围绕 tag、buffer、send 变体编程。
2. 让 codegen 生成物成为主 API，而不是 `FixedLayoutWriter` 的薄包装。
3. 让 runtime loaded profile 与 generated schema 显式绑定并强校验。
4. 让普通线程 send 与 inline callback send 的边界由类型表达，而不是文档约定。
5. 保留当前热路径实现作为 backend，不为 API 改造重写全部性能底座。

### 1.2 非目标

1. 不保留现有 `MessageBuilder` / `SessionHandle` 的公共兼容层。
2. 不把 YAML/TOML/JSON 配置文件格式引入为库级标准配置协议。
3. 不在本阶段推进 router / proxy / HA / Web UI。

---

## 2. 现状问题

当前系统的问题不是“没有 typed API”，而是 typed API 不构成一条完整主路径。

### 2.1 codegen 只覆盖出站编码的一部分

当前 generated writer 实际上是 `FixedLayoutWriter` 的强类型外壳，而不是独立业务消息对象。

证据：

- 当时的 `src/profile/builder_codegen.cpp` 生成类内部直接持有 `message::FixedLayoutWriter`
- 当时的 setter 只是转发到 `writer_.set_*`
- 当时的 group 也是转发到 `writer_.add_group_entry(...)`

这意味着：

- 应用仍然需要理解 layout / dictionary / encode_to_buffer
- generated writer 依然暴露编码器语义
- 它不是业务层 message object

### 2.2 generated reader 不能稳定作为统一读 API

当前 generated reader 顶层字段基于 `MessageView`，但 group 读取走的是 `raw_group()`。

证据：

- 当时的 `src/profile/builder_codegen.cpp`
- 当时的 `include/public/nimblefix/message/message_view.h`

而 `raw_group()` 只对 parser-backed message 有效；owned message 返回空。queue-decoupled 路径会复制消息为 owned message。

证据：

- 当时的 `src/runtime/live_session_worker.cpp`
- 当时的 `src/runtime/live_initiator.cpp`
- 当时的 `src/runtime/live_acceptor.cpp`

因此 generated reader 目前不能作为统一 inbound typed API。

### 2.3 发送表面过度暴露底层细节

当前 `SessionHandle` 公开了多种发送路径：

- `SendCopy`
- `SendTake`
- `SendInlineBorrowed`
- `SendEncodedCopy`
- `SendEncodedTake`
- `SendEncodedInlineBorrowed`

证据：

- 当时的 `include/public/nimblefix/advanced/session_handle.h`

这让应用层必须理解：

- 所有权
- 执行上下文
- 是否预编码
- 生命周期是否合法

这些都不应该成为主业务 API 的一部分。

---

## 3. 新 API 总体原则

### 3.1 schema-first

应用层的业务消息、枚举、view、dispatcher 都由 profile schema 生成。

### 3.2 generated-first

主文档、示例、测试、benchmark 全部以 generated API 为中心。动态/tag-level API 仅保留在 `advanced/`。

### 3.3 session-integrated

generated message object 可以直接发送到 typed session，不需要应用层手工 encode。

### 3.4 bind-before-use

所有 generated profile 在运行时必须通过 `Engine::Bind<Profile>()` 绑定已加载 artifact，校验 `profile_id + schema_hash`。

### 3.5 runtime owns performance strategy

应用层不再决定 layout、template、buffer 等编码策略；这些由 runtime binding 决定。

---

## 4. 目标 Public Header 树

目标是让 public include tree 直接表达主路径。

```text
include/public/nimblefix/
├── runtime/
│   ├── engine.h
│   ├── config.h
│   ├── application.h
│   ├── session.h
│   ├── initiator.h
│   ├── acceptor.h
│   └── profile_binding.h
├── observability/
│   ├── metrics.h
│   └── trace.h
└── advanced/
    ├── dynamic_message.h
    └── raw_passthrough.h
```

generated header 不一定放仓库固定路径，但推荐输出路径为：

```text
generated/
└── fix44.hpp
```

或用户自定义路径，例如：

```cpp
#include "generated/fix44.hpp"
```

### 4.1 从主 public 路径移除的旧头

以下头不再作为主公共面：

- `nimblefix/advanced/message_builder.h`
- `nimblefix/advanced/fixed_layout_writer.h`
- `nimblefix/advanced/typed_message_view.h`
- `nimblefix/advanced/session_handle.h`
- `nimblefix/advanced/encoded_application_message.h`

它们要么下沉到 `advanced/`，要么转 internal。

---

## 5. Generated Header 规范

`dictgen` 新增单一输出选项：

```bash
nimblefix-dictgen --input fix44.nfd --output fix44.nfa --cpp-api generated/fix44.hpp
```

不再保留：

- `--cpp-builders`
- `--cpp-readers`

### 5.1 顶层结构

```cpp
#pragma once

#include "nimblefix/runtime/profile_binding.h"
#include "nimblefix/runtime/session.h"
#include "nimblefix/runtime/application.h"

namespace nimble::fix44 {

struct Profile;
class Handler;
class Dispatcher;

enum class Side : char;
enum class OrdType : char;
enum class OrdStatus : char;

class NewOrderSingle;
class ExecutionReportView;

} // namespace nimble::fix44
```

### 5.2 `Profile` 元数据

每个 generated header 必须有统一的 schema identity 类型。

```cpp
namespace nimble::fix44 {

class Handler;

struct Profile {
  using Application = Handler;

  static constexpr std::uint64_t kProfileId = 4400;
  static constexpr std::uint64_t kSchemaHash = 0x1234567890abcdefULL;
  static constexpr std::string_view kName = "fix44";
};

} // namespace nimble::fix44
```

规则：

- `kProfileId` 和 `.nfa` 一致
- `kSchemaHash` 和 artifact 一致
- bind 阶段强制校验这两个值

### 5.3 generated enum 规范

#### 5.3.1 char enum

```cpp
enum class Side : char {
  Buy = '1',
  Sell = '2',
};

[[nodiscard]] auto ToWire(Side value) -> char;
[[nodiscard]] auto TryParseSide(char wire) -> std::optional<Side>;
```

#### 5.3.2 int enum

```cpp
enum class PartyRole : std::int64_t {
  ExecutingFirm = 1,
  ClientId = 3,
};

[[nodiscard]] auto ToWire(PartyRole value) -> std::int64_t;
[[nodiscard]] auto TryParsePartyRole(std::int64_t wire) -> std::optional<PartyRole>;
```

#### 5.3.3 string enum

```cpp
enum class SecurityType : std::uint16_t {
  CommonStock,
  Future,
};

[[nodiscard]] auto ToWire(SecurityType value) -> std::string_view;
[[nodiscard]] auto TryParseSecurityType(std::string_view wire) -> std::optional<SecurityType>;
```

设计原则：

- public API 返回 enum class，不返回裸 `char` / `int`
- 解析失败返回 `nullopt` 或 `Status`
- 如需保留原始值，单独提供 `*_raw()` accessor

### 5.4 generated outbound message object

generated outbound 类型代表“业务消息对象”，而不是编码器。

#### 5.4.1 顶层对象

```cpp
namespace nimble::fix44 {

class NewOrderSingle {
public:
  static constexpr std::string_view kMsgType = "D";

  auto clear() -> void;

  auto cl_ord_id(std::string_view value) -> NewOrderSingle&;
  auto symbol(std::string_view value) -> NewOrderSingle&;
  auto side(Side value) -> NewOrderSingle&;
  auto transact_time(std::string_view value) -> NewOrderSingle&;
  auto order_qty(std::int64_t value) -> NewOrderSingle&;
  auto ord_type(OrdType value) -> NewOrderSingle&;
  auto price(double value) -> NewOrderSingle&;

  auto add_party() -> PartyEntry&;

  [[nodiscard]] auto validate() const -> base::Status;

private:
  // generated required-field bitset and owned field storage
};

} // namespace nimble::fix44
```

#### 5.4.2 group entry object

```cpp
class PartyEntry {
public:
  auto party_id(std::string_view value) -> PartyEntry&;
  auto party_id_source(PartyIdSource value) -> PartyEntry&;
  auto party_role(PartyRole value) -> PartyEntry&;
};
```

#### 5.4.3 设计规则

- outward API 使用业务字段名，不使用 `set_`
- 不暴露 layout、dictionary、encode buffer
- `clear()` 表示业务对象复用，不表示 encoder state reset
- `validate()` 只校验 required/enum/local object invariants
- encode 不在对象自身上公开；encode 是 runtime binding 的职责

### 5.5 generated inbound typed view

generated inbound view 必须在 parsed / owned storage 下行为一致。

```cpp
namespace nimble::fix44 {

class ExecutionReportView {
public:
  static constexpr std::string_view kMsgType = "8";

  [[nodiscard]] static auto Bind(message::MessageView) -> base::Result<ExecutionReportView>;

  [[nodiscard]] auto order_id() const -> std::string_view;
  [[nodiscard]] auto exec_id() const -> std::string_view;
  [[nodiscard]] auto ord_status() const -> base::Result<OrdStatus>;
  [[nodiscard]] auto symbol() const -> std::optional<std::string_view>;
  [[nodiscard]] auto party_view() const -> std::optional<PartyView>;

private:
  explicit ExecutionReportView(message::MessageView view);
  message::MessageView view_;
};

} // namespace nimble::fix44
```

#### 5.5.1 group view

```cpp
class PartyView {
public:
  class Iterator;

  [[nodiscard]] auto size() const -> std::size_t;
  [[nodiscard]] auto operator[](std::size_t index) const -> PartyEntryView;
  [[nodiscard]] auto begin() const -> Iterator;
  [[nodiscard]] auto end() const -> Iterator;
};

class PartyEntryView {
public:
  [[nodiscard]] auto party_id() const -> std::string_view;
  [[nodiscard]] auto party_role() const -> base::Result<PartyRole>;
};
```

#### 5.5.2 设计规则

- `Bind()` 校验 `MsgType`
- view 基于 `MessageView` / `GroupView` 统一抽象实现
- 不允许因为底层 storage 不同而切到 `raw_group()` 的特殊语义
- enum 字段默认返回 `Result<Enum>`，便于保留 unknown 值错误上下文

### 5.6 generated handler interface

generated header 为该 profile 声明 typed handler 基类。

```cpp
namespace nimble::fix44 {

class Handler : public nimble::runtime::Application<Profile> {
public:
  virtual auto OnExecutionReport(nimble::runtime::InlineSession<Profile>&,
                                 ExecutionReportView) -> base::Status {
    return base::Status::Ok();
  }

  virtual auto OnUnknownMessage(nimble::runtime::InlineSession<Profile>&,
                                message::MessageView) -> base::Status {
    return base::Status::Ok();
  }
};

} // namespace nimble::fix44
```

这样 runtime 不需要知道具体应用派生类型，只需要拿 `Profile::Application` 即可。

### 5.7 generated dispatcher

dispatcher 负责按 `MsgType` 路由并执行 typed bind。

```cpp
namespace nimble::fix44 {

class Dispatcher {
public:
  auto Dispatch(nimble::runtime::InlineSession<Profile>& session,
                message::MessageView message,
                Handler& handler) const -> base::Status;
};

} // namespace nimble::fix44
```

设计规则：

- 先按 `msg_type()` 路由
- 再执行对应 typed view `Bind()`
- bind 失败返回结构化错误
- 未知 `MsgType` 走 `OnUnknownMessage`

---

## 6. 新的 Typed Runtime API

### 6.1 `Application<Profile>`

`runtime/application.h` 提供 profile-agnostic 生命周期基类。

```cpp
namespace nimble::runtime {

template<class Profile>
class Application {
public:
  virtual ~Application() = default;

  virtual auto OnSessionBound(Session<Profile>&) -> base::Status {
    return base::Status::Ok();
  }

  virtual auto OnSessionActive(Session<Profile>&) -> base::Status {
    return base::Status::Ok();
  }

  virtual auto OnSessionClosed(Session<Profile>&, std::string_view) -> base::Status {
    return base::Status::Ok();
  }
};

} // namespace nimble::runtime
```

profile-specific typed app-message callback 由 generated `Handler` 叠加。

### 6.2 `Session<Profile>`

普通应用线程持有的 typed session。

```cpp
namespace nimble::runtime {

template<class Profile>
class Session {
public:
  [[nodiscard]] auto session_id() const -> std::uint64_t;
  [[nodiscard]] auto worker_id() const -> std::uint32_t;
  [[nodiscard]] auto snapshot() const -> base::Result<SessionSnapshot>;

  template<class OutboundMessage>
  auto send(OutboundMessage&& message) -> base::Status;
};

} // namespace nimble::runtime
```

规则：

- 只支持 owning send
- 发送对象必须属于 `Profile`
- 不暴露 encoded / borrowed 变体

### 6.3 `InlineSession<Profile>`

仅在 inline runtime callback 内有效的 typed session。

```cpp
namespace nimble::runtime {

template<class Profile>
class InlineSession {
public:
  [[nodiscard]] auto session_id() const -> std::uint64_t;

  template<class OutboundMessage>
  auto send(const OutboundMessage& message) -> base::Status;
};

} // namespace nimble::runtime
```

规则：

- 只在 runtime inline callback 里生成
- 内部可以走更激进的 zero-copy / borrowed path
- public API 不把这些细节暴露给调用方

### 6.4 `ProfileBinding<Profile>`

bind 对象把 generated schema 与 runtime loaded profile 连接起来。

```cpp
namespace nimble::runtime {

template<class Profile>
class ProfileBinding {
public:
  [[nodiscard]] auto profile_id() const -> std::uint64_t;
  [[nodiscard]] auto schema_hash() const -> std::uint64_t;
  [[nodiscard]] auto dispatcher() const -> const typename Profile::Dispatcher&;
};

} // namespace nimble::runtime
```

bind 阶段职责：

1. 校验 artifact 已加载
2. 校验 `Profile::kProfileId`
3. 校验 `Profile::kSchemaHash`
4. 预构建 encode metadata
5. 预构建 dispatch table

### 6.5 `Engine::Bind<Profile>()`

```cpp
template<class Profile>
auto Engine::Bind() -> base::Result<ProfileBinding<Profile>>;
```

使用方式：

```cpp
nimble::runtime::Engine engine;
engine.Boot(config);

auto fix44 = engine.Bind<nimble::fix44::Profile>();
if (!fix44.ok()) {
  return fix44.status();
}
```

### 6.6 `Initiator<Profile>` / `Acceptor<Profile>`

如果不再保留当前 untyped `LiveInitiator` / `LiveAcceptor` 作为主 public surface，则新的 typed wrapper 应该直接成为主入口。

```cpp
namespace nimble::runtime {

template<class Profile>
class Initiator {
public:
  struct Options {
    std::shared_ptr<typename Profile::Application> application;
  };

  Initiator(Engine* engine, ProfileBinding<Profile>* binding, Options options);
  auto OpenSession(std::uint64_t session_id, std::string host, std::uint16_t port) -> base::Status;
  auto Run() -> base::Status;
};

template<class Profile>
class Acceptor {
public:
  struct Options {
    std::shared_ptr<typename Profile::Application> application;
  };

  Acceptor(Engine* engine, ProfileBinding<Profile>* binding, Options options);
  auto OpenListeners(std::string_view listener_name = {}) -> base::Status;
  auto Run() -> base::Status;
};

} // namespace nimble::runtime
```

---

## 7. 应用代码目标形态

### 7.1 Initiator 示例

```cpp
#include "generated/fix44.hpp"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/initiator.h"
#include "nimblefix/runtime/config.h"

class App final : public nimble::fix44::Handler {
public:
  auto OnSessionActive(nimble::runtime::Session<nimble::fix44::Profile>& session)
    -> nimble::base::Status override {
    nimble::fix44::NewOrderSingle order;
    order.cl_ord_id("ORD-1")
         .symbol("AAPL")
         .side(nimble::fix44::Side::Buy)
         .transact_time("20260428-09:30:00.000")
         .order_qty(100)
         .ord_type(nimble::fix44::OrdType::Limit)
         .price(150.25);
    return session.send(std::move(order));
  }

  auto OnExecutionReport(nimble::runtime::InlineSession<nimble::fix44::Profile>&,
                         nimble::fix44::ExecutionReportView exec)
    -> nimble::base::Status override {
    auto exec_id = exec.exec_id();
    auto ord_status = exec.ord_status();
    (void)exec_id;
    (void)ord_status;
    return nimble::base::Status::Ok();
  }
};

int main() {
  nimble::runtime::EngineConfig config;
  config.profile_artifacts = {"fix44.nfa"};
  config.counterparties = {{
    .name = "buy-side",
    .session = {
      .session_id = 1001,
      .key = nimble::session::SessionKey::ForInitiator("BUY1", "SELL1"),
      .profile_id = nimble::fix44::Profile::kProfileId,
      .is_initiator = true,
    },
  }};

  nimble::runtime::Engine engine;
  auto boot = engine.Boot(config);
  if (!boot.ok()) {
    return 1;
  }

  auto binding = engine.Bind<nimble::fix44::Profile>();
  if (!binding.ok()) {
    return 1;
  }

  auto app = std::make_shared<App>();
  nimble::runtime::Initiator<nimble::fix44::Profile> initiator(
    &engine, &binding.value(), { .application = app });

  auto open = initiator.OpenSession(1001, "127.0.0.1", 9876);
  if (!open.ok()) {
    return 1;
  }
  return initiator.Run().ok() ? 0 : 1;
}
```

### 7.2 Acceptor 示例

```cpp
class SellSideApp final : public nimble::fix44::Handler {
public:
  auto OnNewOrderSingle(nimble::runtime::InlineSession<nimble::fix44::Profile>& session,
                        nimble::fix44::NewOrderSingleView order)
    -> nimble::base::Status override {
    nimble::fix44::ExecutionReport report;
    // fill report...
    return session.send(report);
  }
};
```

关键点：

- 用户不再接触 tag 编号
- 用户不再接触 `MessageBuilder`
- 用户不再接触 `SendCopy/SendTake`
- 用户不再手工 `if (msg_type == "8")`

---

## 8. Internal 实现策略

### 8.1 旧热路径实现继续作为 backend

以下能力可以继续复用：

- `FixedLayoutWriter` 作为 internal encode backend
- `FrameEncodeTemplate` / precompiled template table
- `AdminProtocol` 的 session framing / sequence / store / resend / envelope 逻辑

这意味着 API 大改不等于性能路径重写。

### 8.2 outbound send 内部链路

目标内部流程：

```text
generated outbound object
  -> required-field validation
  -> generated encode adapter
  -> internal fixed-layout or template backend
  -> AdminProtocol final framing
  -> transport write
```

建议实现方式：

1. 每个 generated outbound type 保留紧凑 typed storage
2. codegen 为每个 message 生成 internal `EncodeTraits<Message>`
3. `ProfileBinding<Profile>` 在 bind 时构建 message-type -> layout/template metadata
4. `Session<Profile>::send()` 调用 generic internal encoder，encoder 再通过 `EncodeTraits` 把字段写入 backend

这样 public object 不暴露 encoder，internal 又能保留当前高性能机制。

### 8.3 inbound dispatch 内部链路

目标内部流程：

```text
wire frame
  -> decode to MessageView
  -> runtime determines profile binding
  -> binding.dispatcher().Dispatch(...)
  -> generated typed view Bind()
  -> generated Handler virtual callback
```

### 8.4 shared generated support

应新增共享基础设施，而不是让所有复杂逻辑直接展开进生成头：

- enum parse/render helper
- required-field bitmap support
- message-type bind helper
- storage-agnostic group adapter
- generated dispatch support

建议目录：

```text
include/internal/nimblefix/generated_support/
src/generated_support/
```

生成头只负责：

- 声明类型
- 声明字段名/API
- 调用 shared support

### 8.5 advanced 动态/raw 路径

为代理、协议桥、回放、调试场景保留一条显式高级路径：

- `advanced/dynamic_message.h`
- `advanced/raw_passthrough.h`

但不让这些 API 污染主示例和主文档。

---

## 9. 旧 API 到新 API 的映射

| 旧 API | 新 API | 处理方式 |
|------|------|------|
| `MessageBuilder` | generated outbound object | 下沉到 `advanced/` 或 internal |
| `FixedLayoutWriter` | internal encode backend | 转 internal |
| `TypedMessageView` | generated inbound typed view | 合并到 generated support |
| `SessionHandle::Send*` | `Session<Profile>::send()` / `InlineSession<Profile>::send()` | 删除旧表面 |
| `--cpp-builders` | `--cpp-api` | 删除 |
| `--cpp-readers` | `--cpp-api` | 删除 |
| `ApplicationCallbacks::OnAppMessage(RuntimeEvent)` | generated `Handler::On<Message>()` | 删除主路径 |
| `CounterpartyConfigBuilder` | aggregate init + helper factory | 从主文档移除 |

---

## 10. 迁移后的 public 主路径

迁移完成后，公共主路径应该只需掌握四件事：

1. 用 `dictgen --cpp-api` 生成 profile 头
2. `Engine::Boot(config)`
3. `Engine::Bind<Profile>()`
4. 用 typed `Initiator<Profile>` / `Acceptor<Profile>` + generated `Handler` 编程

也就是从：

```text
tag -> builder -> layout -> encode -> send variant -> callback -> bind typed view
```

变成：

```text
generated message object -> typed session send -> generated typed callback
```

---

## 11. 实施顺序

### 11.1 第一阶段：codegen 重写

- 引入 `--cpp-api`
- 生成 `Profile` / enums / outbound object / inbound view / handler / dispatcher
- 先完成 FIX44 路径闭环

### 11.2 第二阶段：runtime typed binding

- 引入 `ProfileBinding<Profile>`
- 引入 `Engine::Bind<Profile>()`
- 引入 `Session<Profile>` / `InlineSession<Profile>`
- 引入 typed `Initiator<Profile>` / `Acceptor<Profile>`

### 11.3 第三阶段：旧 public API 清理

- 下沉旧消息 API
- 删除旧 send 变体 public surface
- 改写 README / examples / tests / benchmark

### 11.4 第四阶段：恢复非 API 主线能力建设

- config diagnostics
- diagnostics sink
- replay
- dynamic reconfig
- 其他能力项

---

## 12. 测试要求

必须覆盖：

1. codegen compile test
2. outbound object correctness
3. inbound typed view correctness
4. parsed/owned/group consistency
5. typed runtime send path
6. typed runtime callback dispatch
7. schema mismatch failure path
8. benchmark against old generated writer path

The detailed acceptance matrix was tracked outside the checked-in documentation.

---

## 13. 文档更新要求

以下文档必须在主 API 切换后重写：

- `README.md`
- `README_CN.md`
- `docs/public-api.md`
- `docs/architecture.md`

更新要求：

1. 主路径只展示 generated-first API
2. 旧 tag-level API 只出现在 advanced/internal 章节
3. 示例不再出现 `MessageBuilder`、`FixedLayoutWriter`、`SendTake`

---

## 14. 总结

这次重构的核心不是“让 codegen 更强一点”，而是重新定义 NimbleFIX 的公共边界：

- generated schema 成为主 API 真相
- runtime bind 成为 schema 与 artifact 的唯一连接点
- typed session 成为唯一主发送接口
- generated typed handler 成为唯一主接收接口
- 旧 tag/buffer/layout/send-variant 心智全部退出主路径

如果这套设计落地，NimbleFIX 的公共 API 会从“高性能组件拼装包”变成“面向 FIX 业务消息的 typed runtime”。
