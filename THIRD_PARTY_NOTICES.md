# 第三方软件声明

> 适用范围：Industrial-IOT-Gateway v0.1.0 Linux x86_64 软件 MVP  
> 核验环境：Ubuntu 24.04 x86_64  
> 核验日期：2026-08-13  
> 说明：本文是工程分发清单，不构成法律意见。

项目自有代码采用仓库根目录 `LICENSE` 中的 MIT License。该许可证不改变下列第三方软件各自
适用的许可证、版权和免责声明。

## 1. 进入网关运行路径的依赖

| 组件 | 固定包版本 | 许可证记录 | 使用与分发边界 |
|---|---|---|---|
| yaml-cpp | Ubuntu `libyaml-cpp-dev` `0.8.0+dfsg-6build1` | Debian copyright 标为 X11（MIT-style） | `gateway_app` 动态链接；共享库由系统包管理器安装，不随本项目安装包分发 |
| Eclipse Paho MQTT C++ | Ubuntu `libpaho-mqttpp-dev` `1.2.0-2` | Debian copyright 标为 EPL-2.0 | MQTT ON 时动态链接；共享库不随本项目安装包分发 |
| Eclipse Paho MQTT C | Ubuntu `libpaho-mqtt-dev` `1.3.13-1build2` | Debian copyright 标为 EPL-2.0 | Paho C++ 后端动态链接；共享库不随本项目安装包分发 |
| nlohmann/json | Ubuntu `nlohmann-json3-dev` `3.11.3-1` | Expat/MIT | 头文件代码编译进入 MQTT 目标，保留下述版权和许可声明 |
| glibc / libutil | Ubuntu `libc6` `2.39-0ubuntu8.8` | LGPL-2.1-or-later（以系统包 copyright 为准） | Linux 系统动态库，不随本项目安装包分发 |

### 1.1 nlohmann/json MIT/Expat 声明

Copyright (c) 2013-2022 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## 2. 测试专用依赖

| 组件 | 固定包版本 | 许可证记录 | 边界 |
|---|---|---|---|
| GoogleTest | Ubuntu `libgtest-dev` `1.14.0-1` | BSD-3-Clause | 仅构建测试目标，不进入 `gateway_app` 或安装树 |
| Eclipse Mosquitto | Ubuntu `mosquitto` / `mosquitto-clients` `2.0.18-1build3` | EPL-2.0 OR EDL-1.0 | 测试专用回环 broker/客户端，不进入网关运行安装包 |

## 3. 构建工具

GCC/Clang、CMake、Ninja 和 CTest 用于构建、静态检查或执行测试，不链接为项目内置组件。
它们不应在 Release manifest 中被误写为随 `gateway_app` 分发的库。

## 4. 来源与完整文本

- yaml-cpp：<https://github.com/jbeder/yaml-cpp>
- Eclipse Paho MQTT C++：<https://github.com/eclipse-paho/paho.mqtt.cpp>
- Eclipse Paho MQTT C：<https://github.com/eclipse-paho/paho.mqtt.c>
- nlohmann/json v3.11.3：<https://github.com/nlohmann/json/blob/v3.11.3/LICENSE.MIT>
- GoogleTest：<https://github.com/google/googletest>
- Eclipse Mosquitto：<https://github.com/eclipse-mosquitto/mosquitto>
- 已安装 Ubuntu 包的精确声明：`/usr/share/doc/<package>/copyright`

如果后续 Release 把任何第三方共享库复制进 bundle，必须按实际复制内容重新复核并附带相应
完整许可证/NOTICE；不能继续沿用“系统包不随包分发”的边界。
