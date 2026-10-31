# NimbleFIX 0.2 入门

当前实现包含完整 FIX44 生成绑定、group、typed 接收、Inline/Queued、MPSC Sender、
分段 journal、TLS、认证、TOML/CLI、运行时 XML、自定义字典和手动驱动。
[支持表](session-support.md)说明具体协议和资源边界，[发布记录](release-0.2.md)保存验收证据。

## 第一次成功收发

```sh
xmake f -m release
xmake build hello-fix
xmake run hello-fix --self-test --typed --restart
```

此命令启动真实 TCP 双方，完成 Logon、订单、成交报告和 Logout，再保留状态重启。
独立进程示例：

```sh
xmake build acceptor-example initiator-example
xmake run acceptor-example --state ./state/sell --port 9876
# 另一个终端：
xmake run initiator-example --state ./state/buy --port 9876 --cl-ord-id DEMO-001
```

保留两端各自的状态目录；新订单使用新的 ClOrdID。回调故障恢复和应用外部副作用的幂等由应用负责。

## 使用配置文件

```sh
xmake build nimblefix-cli
xmake run nimblefix-cli init --directory ./demo
xmake run nimblefix-cli validate --config ./demo/acceptor.toml --json
xmake run nimblefix-cli run --config ./demo/acceptor.toml --json
# 另一个终端：
xmake run nimblefix-cli run --config ./demo/initiator.toml --json
```

CLI 接收并统计业务消息。要发订单／回复成交报告，运行前面的业务示例或嵌入库。
配置 `version = 1`；相对路径基于配置文件所在目录；未知键、非法值和不支持的组件均报错。
`validate` 只读配置、字典和证书，不监听、不连接、不打开 journal。SIGINT/SIGTERM 触发
有期限的 Logout，退出码保留 `128 + signal`。

```toml
version = 1
[engine]
dispatch = "queued" # 或 inline
schema = "fix44"   # none 用于显式 flat 路径
[engine.storage]
directory = "./state/sell"
confirmation = "appended"
segment_bytes = 1048576
capacity_bytes = 16777216
retain_outbound_messages = 8192
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

`init` 生成一对可运行文件；不覆盖已有文件，除非明确传 `--force`。
可用 `doctor --json` 检查构建能力，`inspect frame --path file --json` 检查帧，
`inspect journal --path journal --session-key 'FIX.4.4|4:SELL|3:BUY' --json`
只读检查存储，不修复或抢占 writer。Logon 敏感字段不会出现在 inspect 输出或 journal payload。

## TLS 与认证

```sh
xmake f -m release --tls=y --xml=y
xmake build nimblefix-cli
```

TLS 使用本机 OpenSSL 3；XML 使用本机 libxml2。构建不会下载依赖。
在 acceptor 的 `[[listeners]]` 后、`[[sessions]]` 前添加：

```toml
[listeners.tls]
enabled = true
certificate_file = "./certs/server.pem"
private_key_file = "./certs/server-key.pem"
ca_file = "./certs/ca.pem"
require_client_certificate = true
```

initiator 使用 `[sessions.tls]`，设置 `enabled = true`、`ca_file`、客户端证书和私钥，
以及匹配服务器 SAN 的 `server_name`。服务器身份验证失败会关闭连接，不降级明文。
私钥必须能非交互读取；不在 owner 上提示输入口令。

acceptor 使用显式环境变量作为密码来源：

```toml
[sessions.authentication]
method = "credentials"
username = "demo"
password_env = "VENUE_PASSWORD"
require_tls = true
```

initiator 对应 `[sessions.logon]` 的 `username` 和 `password_env`。
其他 Logon 字段可用 `[[sessions.logon.fields]]` 的 `tag`、`value` 或 `value_env`、`sensitive`。
没有全局环境变量替换。C++ 还支持认证回调，运行在会话接入边界，必须有界。
认证先于 acceptor journal 创建。

## 业务代码与线程

默认 `Engine<>` 是 flat + Inline；`Engine<fix44::Schema>` 使用完整 typed FIX44。
生成消息支持具名 setter、`set<Tag>()`、具名 getter、`get<Tag>()` 和 group。
运行期 tag 使用 `DynamicMessage`，不会让静态 builder 偷偷改变形状。

`Engine<Schema, Queued>` 将业务回调放到有限 worker pool。同一 session 串行投递；
依赖回复被 owner append 后才提交 delivery-complete。普通 `SessionHandle::sender<Schema>()`
可供多个线程使用；显式 producer lane 用于高级单生产者场景，跨线程误用报 `wrong_thread`。

Inline 的 `context.send` 成功为 appended；Queued／Sender 首先为 accepted，必须查询 receipt
观察 appended/written/failed/expired。失败不消费 builder，也不推进 FIX 序号。
written 是 transport 接受字节，不等于对手收到或业务接受。

回调的 view 和 SessionContext 只在该回调中有效；需要保留数据时深拷贝。
不能保存 context 跨线程发送。`run()` 在调用线程拥有协议状态；嵌入外部事件循环时使用
`start()` 加同线程 `poll_once(max_wait_ms)`，直到 `running()` 为 false。
其他线程可 `request_stop()`、查询 snapshot；错误 observer 在单独有界诊断通道执行。

## 恢复与资源

journal 使用 appended mmap queue，input 先入日志再投递；未完成输入在重启后以
`is_recovery()` 再次投递。依赖回复必须按原顺序重建相同内容，已提交输出可安全复用。
这覆盖 OS／文件系统仍工作的同机进程死亡，不承诺断电落盘；没有逐消息 fsync/msync。

分段、索引和 checkpoint 有显式格式与 CRC。清理同时服从 peer replay 进度、完成投递和 reader／lease。
空间不足报 `storage_full`，不覆盖仍需恢复的数据。journal 失败使相应 session 停止；不会删库或从 1 偷偷重来。
显式 `allow_sequence_reset` 加 Logon `reset_sequence` 才能开启新 epoch；设置为 true
表示每次新连接请求 reset，必须与对手约定。定时开关 session 不自动 reset。

时间窗口为 UTC，`[sessions.schedule]` 可设置 `weekdays = [1,2,3,4,5]`、
`start_utc = "09:00:00"`、`end_utc = "17:00:00"`；跨午夜窗口归起始日。
流量限制通过 `[sessions.flow_control]` 显式设置消息数、字节数、投递年龄、暂停和写停滞期限。
回调必须返回，框架不会抢占任意 C++ 用户代码。

## 自定义 XML 与安装

```sh
xmake run nimblefix-cli schema generate --xml venue.xml --namespace venue --output venue.hpp
xmake run nimblefix-cli schema summary --xml venue.xml --json
xmake check
python3 tests/install_test.py --features --examples
python3 tests/xmake_schema_test.py
```

标准字典包含 FIX42、FIX43、FIX44、FIXT11 和 FIX50SP2。运行时 XML 使用独立可选组件；
若同时选择生成 binding 和 XML，自动核对 fingerprint。修改 XML 后由构建 helper 增量生成，
无需手动维护 hash。CMake 使用 `nimblefix_generate_schema`，xmake 使用安装的
`share/nimblefix/xmake/nimblefix-schema.lua` 规则。

同一 CompID／transport 的本地 qualifier 可区分持久化身份；acceptor 需要独立 listener 来消除入站路由歧义。

0.2.x 冻结公开 source API；升级后重编应用，不承诺 C++ ABI。普通 Engine 默认 v2 分段格式；
底层 Journal 的 `segment_bytes = 0` 保留 v1 行为，不做隐式文件迁移。
