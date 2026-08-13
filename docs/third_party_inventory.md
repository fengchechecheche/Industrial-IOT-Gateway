# 第三方依赖清单

> 固定环境：Ubuntu 24.04 x86_64  
> 核验日期：2026-08-13  
> Release 声明入口：[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)

| 依赖 | Ubuntu 来源与版本 | 许可证记录 | 使用范围 | 交付边界 | 验证结果 |
|---|---|---|---|---|---|
| GoogleTest | `libgtest-dev` `1.14.0-1` | BSD-3-Clause | 测试专用 | 不进入 `gateway_app` 和安装树 | CMake 发现及单元/集成测试通过 |
| yaml-cpp | `libyaml-cpp-dev` `0.8.0+dfsg-6build1` | X11（Debian copyright 标识，MIT-style） | YAML 配置解析 | `gateway_app` 动态链接；共享库不随项目包分发 | 冻结寄存器表和场景配置成功加载 |
| glibc `openpty` / `libutil` | `libc6` `2.39-0ubuntu8.8` | LGPL-2.1-or-later | PTY 测试/仿真 | 系统动态库，不随项目包分发 | `<pty.h>` 探测、链接和真实 PTY 测试通过 |
| Eclipse Paho MQTT C++ | `libpaho-mqttpp-dev` `1.2.0-2` | EPL-2.0（Ubuntu 包记录） | MQTT C++ 异步客户端 | MQTT ON 时动态链接；共享库不随项目包分发 | QoS 1、断线/重连和关闭测试通过 |
| Eclipse Paho MQTT C | `libpaho-mqtt-dev` `1.3.13-1build2` | EPL-2.0（Ubuntu 包记录） | Paho C++ 传输后端 | 动态链接；共享库不随项目包分发 | MQTT ON 集成测试通过 |
| nlohmann/json | `nlohmann-json3-dev` `3.11.3-1` | Expat/MIT | MQTT payload JSON | 头文件代码编译进入 MQTT 目标；声明进入 NOTICE | 黄金 payload 合同通过 |
| Eclipse Mosquitto | `mosquitto` / `mosquitto-clients` `2.0.18-1build3` | EPL-2.0 OR EDL-1.0 | 测试专用回环 broker/客户端 | 不进入 `gateway_app` 和安装树 | broker 停止/恢复及 PTY 持续轮询通过 |

## 分层结论

- 本仓库未复制 yaml-cpp、Paho、GoogleTest 或 Mosquitto 的第三方源代码；
- nlohmann/json 是头文件依赖，其代码会编译进入 MQTT 目标，因此在
  `THIRD_PARTY_NOTICES.md` 中保留版权与 MIT/Expat 文本；
- yaml-cpp、Paho C++/C 和 glibc 在当前 Linux x86_64 方案中由 Ubuntu 包管理器提供并动态链接；
- GoogleTest 和 Mosquitto 仅用于测试，不属于网关运行安装树；
- GCC、Clang、CMake、Ninja 和 CTest 是构建/检查工具，不写成网关内置组件；
- `gateway_core` 不公开 Paho 类型，只有 `GATEWAY_ENABLE_MQTT=ON` 时才构建
  `gateway_mqtt`；
- 软件依赖和 PTY 结果均不构成真实 RS485 电气验证证据。

如果 T04 把任何第三方 `.so` 复制进正式 bundle，必须根据实际文件重新生成依赖 manifest 和
许可证集合，不能继续沿用“动态系统包不随包分发”的结论。本文是工程盘点，不构成法律意见。
