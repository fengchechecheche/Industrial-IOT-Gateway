# 第三方依赖清单

| 依赖 | 来源 | 版本 | 许可证 | 使用范围 | 验证结果 |
| ---- | ---- | ---- | -------- | -------- | -------- |
| GoogleTest | Ubuntu `libgtest-dev` 软件包 | 1.14.0 | BSD-3-Clause | 仅用于测试目标 | CMake 已找到；smoke test 通过 |
| yaml-cpp | Ubuntu `libyaml-cpp-dev` 软件包 | 0.8.0 | MIT | 仅供 `pty_slave_support` 读取冻结 YAML 配置 | 三个冻结从站配置均成功加载；无内置源码 |
| glibc `openpty` / `libutil` | Ubuntu 系统组件 | 2.39 | LGPL-2.1-or-later | `pty_slave` 和 PTY 进程级集成测试 | `<pty.h>` 配置探测、链接和真实 PTY 测试通过 |

本仓库未内置任何第三方源代码。GCC、CMake、Ninja、Clang 工具和 CTest 均属于构建工具，不会链接到项目交付物中。

MQTT、JSON 和独立日志库仍未引入。`P3-S2-T05` 按批准方案引入 yaml-cpp；PTY
直接使用系统 `openpty/libutil`，不依赖 `socat`。以上依赖不作为真实 RS485 电气验证证据。
