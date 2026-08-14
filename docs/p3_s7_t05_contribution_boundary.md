# P3-S7-T05：工程贡献边界候选稿

> 状态：用户审核通过，已授权同步独立仓库正式维护版本  
> 审核日期：2026-08-15  
> 冻结输入：`a1fbcdc7661c6fee61c688259ee222872a3cce87`  
> 适用范围：简历候选、项目介绍和面试回答的事实边界  
> 发布范围：Linux x86_64 软件 MVP；ARM64 仅完成原生构建与 systemd 软件验收

## 1. 本文解决什么问题

这个项目同时使用个人编写的网关代码、第三方开源库、Linux 平台能力和测试环境。本文把四者分开，避免把“完成集成”写成“从零实现协议栈”，也避免把 PTY 软件验证写成真实 RS485/CAN 台架验证。

权威声明 ID 与证据关系见 `P3-S7-T05_claim_ledger.yaml`；本文是面向人的解释版本。

## 2. 可归入个人工程实现的内容

以下内容由项目源码和合同证明已经实现，但只有存在正式测试结果的部分才能附带量化结论：

- Modbus RTU `0x03`、`0x04`、`0x06` 子集的 CRC16、编解码、流式解析、异常响应和错误分类；
- 3 从站、16 点位配置的数据类型、缩放、质量和 MQTT 映射；
- termios 非阻塞串口封装、单串口所有者、多从站轮询与显式公平性；
- 超时、重试、退避以及 fresh、stale、offline、invalid 质量转换；
- 有界请求/发布队列、普通遥测按 topic 合并、关键状态保留和有界关闭；
- MQTT 消息合同、JSON 序列化、Paho 适配、重连策略和运行指标；
- PTY 三从站仿真、G3 故障 runner、G4 长稳 runner、x86_64/ARM64 systemd 十场景 runner；
- CMake/CTest、Sanitizer、clang-tidy、格式检查、安装暂存、manifest、规范化归档和复演证据链。

允许表述为“实现”“设计”“集成”“构建”“验证”。若提到性能或通过率，必须同时带平台与测试环境限定。

## 3. 第三方库与平台能力

| 能力 | 本项目做了什么 | 不得表述为 |
|---|---|---|
| Eclipse Paho MQTT C/C++ | 集成异步客户端，封装 publish sink、回调、重连和关闭 | 自研 MQTT 协议栈 |
| Mosquitto | 用作本地匿名 broker 和测试客户端 | 自研 MQTT broker |
| yaml-cpp | 解析 YAML 配置 | 自研 YAML 解析器 |
| nlohmann/json | 构建与序列化 JSON | 自研 JSON 库 |
| GoogleTest/CTest | 测试表达与编排 | 自研测试框架 |
| Linux PTY/termios | 提供伪终端和串口 API | 实现了 Linux 串口驱动；PTY 等同 RS485 |
| systemd/journald | 提供服务管理、信号、重启和日志平台 | 自研服务管理器 |
| GCC/Clang/CMake/Ninja | 构建与质量工具 | 项目运行时代码 |

项目自有代码采用 MIT License；第三方依赖的版本、许可证和使用方式以 `THIRD_PARTY_NOTICES.md` 为准。

## 4. 已验证结果与准确适用范围

| 结果 | 可以声称 | 必须保留的边界 |
|---|---|---|
| G3 | 15/15 软件故障场景 PASS | Linux PTY、本地 Mosquitto；不是物理总线故障 |
| G4 | Linux x86_64 单次 8 小时、224/224 强制 oracle PASS | PTY/本地 Mosquitto；不外推工业现场可靠性 |
| x86_64 G5 | VMware Ubuntu Server 24.04 原生 systemd 十场景 10/10 PASS | 虚拟机软件服务验收 |
| x86_64 bundle | 9 文件、双目录规范化归档一致、清洁解包/卸载复演 PASS | bundle ready，未创建 tag 或公开 Release |
| ARM64 T01 | Raspberry Pi 4B 原生构建与质量门，Release CTest 191/191 PASS | 未做 ARM64 8 小时长稳或 bundle |
| ARM64 T02 | Raspberry Pi 4B 原生 systemd 十场景 10/10 PASS | 软件服务验收，不含 RS485/CAN |

## 5. 尚未完成的能力

- USB-RS485、商用 Modbus 从站、STM32 和 CAN 实物台架；
- RS485 终端、偏置、隔离、浪涌、长线、EMC 和计量准确度验证；
- ARM64 8 小时长稳、断电/重启开机自启演练和 ARM64 Release bundle；
- TLS、生产凭据、云平台、远程运维和安全加固；
- MQTT 下行写寄存器、完整 Modbus 功能集、Modbus TCP 和工业认证；
- 正式 Git tag、GitHub Release 和安装包公开分发。

这些内容必须写为“待执行”“未覆盖”或“下一步”，不能用 x86_64、PTY 或 systemd 结果替代。

## 6. 推荐与禁止表述

### 6.1 推荐表述

- “基于 Eclipse Paho 集成 MQTT 3.1.1 QoS 1 异步上行。”
- “在 Linux x86_64 PTY/本地 Mosquitto 环境完成 8 小时软件长稳。”
- “在 Raspberry Pi 4B / Ubuntu Server 24.04 ARM64 完成原生构建和 systemd 软件验收。”
- “形成 Linux x86_64 可复现 bundle，尚未创建 tag 或公开发布。”

### 6.2 禁止表述

- “自研 MQTT 协议栈/broker/YAML 解析器/JSON 库/systemd。”
- “完成真实 RS485/CAN、STM32 或商用传感器验证。”
- “达到工业级、生产可用、现场验证或高可靠。”
- “ARM64 8 小时长稳和 ARM64 Release 已完成。”
- “v0.1.0 已正式发布”或“GitHub Release 已上线”。

## 7. 硬件到货后的增量原则

硬件结果应新增独立事实和 claim，不能回写或重命名旧的软件证据：

1. 冻结设备型号、接线、供电、串口参数、固件和候选提交；
2. 生成新的硬件 run ID、场景矩阵、summary、failures 和哈希；
3. 将真实硬件结果标为新的 `VERIFIED_RESULT`；
4. 继续保留 G3/G4/G5 的 PTY、x86_64 和 ARM64 软件边界；
5. 只有真实完成的设备和场景进入简历，未运行项目继续保留 `NOT_RUN_OPTIONAL`。

## 8. 当前结论

当前可可信地介绍为“完成 Linux x86_64 软件 MVP，并在 Raspberry Pi 4B ARM64 上完成原生构建和 systemd 软件集成验证”。不能介绍为“真实工业硬件网关已经完成现场验证或公开发布”。
