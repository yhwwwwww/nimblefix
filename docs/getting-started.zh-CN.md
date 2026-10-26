# Codec / slot 切片使用说明

当前实现是 FIX 4.4 无状态编解码器和紧凑消息 slot。`Engine`、真实 TCP、
mmap journal、序号恢复与完整 FIX44 支持按[设计顺序](rewrite-design.md#16-實作順序與完成條件)
继续开发。此阶段的 `codec-demo` 在内存缓冲区完成编码与解析。

需要 Linux、C++20 编译器和 xmake；XML 生成及测试另需 Python 3.10 以上。
核心没有第三方依赖，构建时不下载内容。

```sh
xmake f -m release
xmake check
xmake run codec-demo
```

可编译示例在 [examples/codec.cpp](../examples/codec.cpp)。
不提供字典即可使用 `message<"D">().set<11>(...)` 声明字段；字段值可以在
运行时产生，tag 必须在编译时确定。flat 消息按声明顺序输出，不检查业务必填条件。
`encode_fix44` 接受明确的 header 和调用方提供的连续缓冲区，返回写入长度或错误；
失败时缓冲区保持原状。价格可使用 `"150.25"_dec`，避免经过二进制浮点数。

setter 返回前会复制字符串。fluent builder 是 move-only，默认最多 64 个字段、
4096 字节值内容，其存储增长有堆分配。需要固定容量和重复使用时，使用
`FixedMessage<MsgType, Fields<...>, ByteCapacity>`：slot 数只取决于声明字段数，
`reset()` 清除 presence、已用字节数和错误；固定形状的填值与编码没有堆分配。
错误会保留到 reset；调用方必须检查 `Status` 或编码结果。

`inspect_frame` 区分消息未完整和格式错误，并从合并输入中返回第一条 frame。
`parse_flat` 使用调用方提供的 `FieldView` 数组，保留顺序和重复字段。
`find_unique(tag)` 分别返回缺失、重复或唯一字段。view 借用 frame 和索引数组，
两者在使用期间必须保持有效且不可更改。标准 Length/Data 配对允许接收二进制值中
含 SOH；自定义配对通过 `WireHints` 提供。不凭 tag 或计数猜测 repeating group。

可选 XML 生成器只实现 flat 消息的最小子集，当前支持 `STRING`、`CHAR`、`INT`、`PRICE`、
`QTY`、`UTCTIMESTAMP`，校验必填字段、类型和枚举。生成的名称 setter 与静态
`set<Tag>()` 共用 slot、校验和编码器，并按 XML 顺序输出。输入限制、冲突、未知引用、
不支持的类型、group 或 component 都返回明确错误。样本不是完整 FIX44 字典，
尚未提供 typed 接收 getter 和条件业务规则。

```sh
python3 tools/schema/generate.py --namespace venue \
  --output build/generated/venue.hpp schemas/fix44-sample.xml
```

安装和外部 CMake 使用方：

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j2
cmake --install build-cmake --prefix /tmp/nimblefix-install
python3 tests/install_test.py
```

外部工程使用 `find_package(NimbleFIX 0.1 CONFIG REQUIRED COMPONENTS core)` 和
`NimbleFIX::nimblefix`。安装的 `nimblefix_generate_schema` helper 会跟踪 XML 修改并
重新生成 C++。核心单独构建可关闭 `NIMBLEFIX_BUILD_TESTS` 与
`NIMBLEFIX_BUILD_SAMPLE_SCHEMA`，从而不依赖 Python。

原始 codec 数据、已验证的不变量、审核结果和后续门槛见[切片验收记录](codec-slice.md)。
这些量测不包含 session、存储或网络，尚未完成 QuickFIX 独立互通与专用机性能验收。
