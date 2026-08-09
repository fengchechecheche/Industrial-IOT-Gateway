# 第三方依赖清单

| 依赖 | 来源 | 版本 | 许可证 | 使用范围 | 验证结果 |
| ---- | ---- | ---- | -------- | -------- | -------- |
| GoogleTest | Ubuntu `libgtest-dev` 软件包 | 1.14.0 | BSD-3-Clause | 仅用于测试目标 | CMake 已找到；smoke test 通过 |
| yaml-cpp | Ubuntu `libyaml-cpp-dev` 软件包 | 0.8.0 | MIT | 运行时与 `pty_slave_support` 读取冻结 YAML 配置 | 三个冻结从站配置和运行时寄存器表均成功加载；无内置源码 |
| glibc `openpty` / `libutil` | Ubuntu 系统组件 | 2.39 | LGPL-2.1-or-later | `pty_slave` 和 PTY 进程级集成测试 | `<pty.h>` 配置探测、链接和真实 PTY 测试通过 |
| Eclipse Paho MQTT C++ | Ubuntu `libpaho-mqttpp-dev` | 1.2.0-2 | EPL-2.0 / EDL-1.0 | `gateway_mqtt` 的 C++ 异步客户端接口 | MQTT ON 构建、断线/重连、QoS 1 和关闭测试通过 |
| Eclipse Paho MQTT C | Ubuntu `libpaho-mqtt-dev` | 1.3.13-1build2 | EPL-2.0 / EDL-1.0 | Paho C++ 的异步传输后端 | 作为共享库动态链接；未内置源码 |
| nlohmann/json | Ubuntu `nlohmann-json3-dev` | 3.11.3-1 | MIT | MQTT payload JSON 序列化与测试解析 | fresh/stale/offline/invalid/status/health 合同测试通过 |
| Eclipse Mosquitto | Ubuntu `mosquitto`、`mosquitto-clients` | 2.0.18-1build3 | EPL-2.0 / EDL-1.0 | 仅用于回环 broker 集成测试 | broker 停止/恢复与 PTY 持续轮询测试通过 |

本仓库未内置任何第三方源代码。GCC、CMake、Ninja、Clang 工具和 CTest 均属于构建工具，不会链接到项目交付物中。

`P3-S4-T02` 没有把 Paho、nlohmann/json 或 Mosquitto 源码复制进仓库。`gateway_core`
不公开 Paho 类型；只有启用 `GATEWAY_ENABLE_MQTT` 时才构建并链接 `gateway_mqtt`。
Mosquitto 不进入网关交付物，只是 CTest 的本地测试进程。以上软件依赖和 PTY 结果均不作为
真实 RS485 电气验证证据。
