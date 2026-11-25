# NimbleFIX 0.3 中文入门（已整合至《使用手册》）

> 本页面已升级并整合至完整的双语使用手册：
> - **[使用手册（简体中文）](user-guide.zh-CN.md)**
> - **[User Guide (English)](user-guide.md)**
> - **[README（简体中文）](../README.zh-CN.md)** · **[README (English)](../README.md)**

完整手册包含：
- 纯 C++ 配置设计哲学（零配置文件格式偏好）与三层按需链接选型
- 零堆分配流式编码（`StreamWriter`）与强类型视图示例
- 4 大核心业务场景的推荐写法（单线程 `DirectInitiator`、双线程无锁 `DirectSendQueue`、多会话 `Engine<Inline|Queued>`、多核 `ShardedEngine`）
- 各使用模式下的详细线程模型与对象线程安全性速查表
- 用户自行管理 vs NimbleFIX 库自动管理的完整职责边界矩阵
- 零 Python 的原生 C++20 字典编译器（`nimblefix-schema-gen`）、TLS/mTLS、主备切换、旁路 mmap 遥测与历史回放实战指南
