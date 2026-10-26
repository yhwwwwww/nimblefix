# 第一笔 FIX 收发

当前实现覆盖设计阶段 1–3：codec / slot、appended mmap journal、inline session，
以及真实 TCP initiator / acceptor。使用 Linux、C++20 编译器、xmake 2.9.8 以上；
生成样本和运行测试需要 Python 3.10 以上。核心不下载第三方依赖。

## 本机登录、下单、回报、登出和重启

```sh
xmake f -m release
xmake build hello-fix
xmake run hello-fix --self-test --restart
xmake run hello-fix --self-test --typed --restart
```

程序创建临时存储目录及真实 TCP 两端，发送 NewOrderSingle，验证 ClOrdID、数量、
方向等对应的 New ExecutionReport，完成 Logout。`--restart` 重新打开原 journal，
验证两端序号继续增长；测试结束后清理临时目录。`--typed` 使用 XML 生成的样本
builder 发消息；接收仍通过 `Engine<>` 的 `FlatMessageView`。

## 分别运行 initiator 和 acceptor

```sh
xmake build
# 终端一
xmake run acceptor-example --state ./state/sell --port 9876
# 终端二
xmake run initiator-example --state ./state/buy --port 9876 --cl-ord-id DEMO-001
```

acceptor 持续服务，按 Ctrl-C 提出有期限的停止请求。initiator 收到对应回报后自行退出，
默认等待回报 10 秒；`--timeout-ms` 可调整。`--host` 支持数字 IPv4/IPv6 地址。
保留各自目录即可恢复协议序号与重发数据；目录必须可写，一个 session 只允许一个 writer。
新 initiator 进程会提交新订单。订单业务唯一性由应用和 ClOrdID 策略负责，恢复协议
不会替应用决定是否再次下单。

## 编写应用

可编译的完整程序是 [initiator.cpp](../examples/initiator.cpp) 和
[acceptor.cpp](../examples/acceptor.cpp)。公开 API 的顺序为：

1. `Engine<>::create()` 指定明确的 `storage.directory`。
2. 注册 `on_message`、`on_session_active`、`on_error`，检查返回值。
3. initiator 调用 `add_initiator`；acceptor 调用 `add_listener` 和 `add_acceptor`。
4. `validate()` 做离线检查，再由一个 owner 线程调用 `run()`。
5. 在 active/message callback 中使用 `context.send(message)`，其他线程可调用 `request_stop()`。

`context.send()` 成功表示编码完成且 journal 已提交，尚不表示 socket 写完或对手方收到。
拒绝时不消耗调用方消息、不推进序号。`SessionDriver` 提供 appended/written/failed
receipt 查询和容量恢复回调；当前 `Engine` 尚无普通跨线程 Sender，后者属于阶段 4。
上下文、接收 view 和字段字符串只在当前 callback 有效，不可保存引用。
callback 返回成功后记录 delivery-complete；失败保留未完成交付并隔离该 session。

崩溃后未完成的 callback 会重新执行，`context.is_recovery()` 和
`inbound_record_id()` 可用于恢复处理。回复按该输入及 send 次序关联；恢复时必须重复
相同回复，已提交的回复会复用原序号，变化或遗漏会报错。例子复用订单 Tag60 和输入
record id 生成稳定回报。应用的外部数据库操作还需要自己的幂等或事务设计。

inline callback 必须有界；终端输出在示例的另一个线程通过有界事件队列处理。
`request_stop()` 唤醒 owner，尝试 Logout，期限到达后回收连接。检查 `run()` 的结果和
error event；socket 写完、Logout 成功都不等同于交易业务处理确认。

## 消息与字典

无字典时用 `message<"D">().set<11>(...)`；tag 在编译期确定，值可在运行时产生。
setter 返回前复制字符串。默认 fluent builder 最多 64 字段、4096 字节值内容，
存储增长有堆分配；`FixedMessage<...>` 使用固定 arena，可 `reset()` 后重复使用。
价格用 `"150.25"_dec` 保持精确十进制。flat 模式不验证业务必填规则。

XML 生成支持 `STRING`、`CHAR`、`INT`、`PRICE`、`QTY`、`UTCTIMESTAMP`，验证必填、
枚举和类型。样本只含两种消息，名称 setter 和静态 tag 共用 slot / encoder。
完整 FIX44、typed receive、group、queued 和 MPSC 属于阶段 4。
当前配置不提供认证、TLS、session 时段、store 轮转，详见[支持表](session-support.md)。

`inspect_frame` 处理半包及粘包；`parse_flat` 保留字段顺序和重复值，
`find_unique` 区分缺失和重复。二进制 Length/Data 配对可按标准表或 `WireHints` 解析，
不凭 tag 或计数推断 group。更详细的所有权和容量限制见 [codec 契约](codec-slice.md)。

## 安装及外部工程

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j2
cmake --install build-cmake --prefix /tmp/nimblefix-install
cd /tmp
/tmp/nimblefix-install/bin/hello-fix --self-test --restart
```

外部 CMake 工程使用 `find_package(NimbleFIX 0.1 CONFIG REQUIRED COMPONENTS core)` 和
`NimbleFIX::nimblefix`。安装的 `nimblefix_generate_schema` 跟踪 XML 修改并自动重新生成。
关闭 `NIMBLEFIX_BUILD_TESTS` 与 `NIMBLEFIX_BUILD_SAMPLE_SCHEMA` 可移除核心构建对 Python
的依赖；生成代码不要求运行时部署 XML。

```sh
xmake check
python3 tests/install_test.py --examples
python3 tests/install_test.py --core-only --examples
```

安装验收使用只读源码副本，迁移安装目录、隐藏原源码和 build，然后编译外部使用方，
并在其他工作目录运行安装后的 TCP 例子。[阶段 2–3 记录](session-tcp-slice.md)保存
恢复、背压、公平性、独立对手方及性能基线的证据和边界。
