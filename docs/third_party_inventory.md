# 第三方依赖清单

| 依赖 | 来源 | 版本 | 许可证 | 使用范围 | 验证结果 |
| ---- | ---- | ---- | -------- | -------- | -------- |
| GoogleTest | Ubuntu `libgtest-dev` 软件包 | 1.14.0 | BSD-3-Clause | 仅用于测试目标 | CMake 已找到；smoke test 通过 |

本仓库未内置任何第三方源代码。GCC、CMake、Ninja、Clang 工具和 CTest 均属于构建工具，不会链接到项目交付物中。

MQTT、YAML、JSON、日志和 PTY 辅助依赖均不属于 `P3-S2-T01` 范围；只能在获批准的实现阶段引入，并同步更新本清单。
