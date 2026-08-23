# Industrial-IOT-Gateway 项目冻结记录

> 生命周期状态：`PROJECT_FROZEN / MAINTENANCE_ONLY`
>
> 生效规则：首次同时包含本文档和补充证据包的 Git 提交构成有效冻结基线
>
> 冻结父提交：`18e0af7c57e916af641e1e868995cb1a4855a631`
>
> 冻结生效提交：首次同时包含本文档和
> `docs/releases/v0.1.0_hardware_validation_supplement/` 的 Git 提交
>
> 编制日期：2026-08-23

## 1. 冻结结论

项目的软件 MVP、本地双架构 Release、Raspberry Pi ARM64 原生运行、systemd、故障恢复和真实
三从站 G6 验收均已关闭。轻量 v0.1.0 硬件验证补充证据包只封装既有结果，没有重建软件包，
也没有重复执行一小时预跑、八小时长稳、17 场景故障矩阵或 reboot。

用户提交本次冻结文件后，项目状态转为：

```text
PROJECT_FROZEN
MAINTENANCE_ONLY
LOCAL_DUAL_ARCH_RELEASE_READY
POST_RELEASE_G6_HARDWARE_SUPPLEMENT=PASS
PUBLISHED=false
TAG=null
```

## 2. 已冻结交付范围

- C++17 Modbus RTU 网关、CRC、编解码、流式解析和多从站调度；
- Linux 非阻塞串口、超时/重试/隔离、新鲜度、有界队列和结构化日志；
- MQTT 3.1.1 QoS 1 异步上行、缓存、重连、LWT 和关闭协议；
- PTY/Mosquitto 软件故障矩阵和长稳证据链；
- Ubuntu 24.04 x86_64 与 Raspberry Pi 4B ARM64 的 v0.1.0 本地双架构软件包；
- 两只 TAS-WS-R00020（地址 1/2）、STM32（地址 4）、19200 8E1 的真实三从站台架；
- 17/17 个真实故障/systemd 恢复场景；
- 3600 秒预跑和 28800 秒正式真实硬件长稳；
- G6 evidence matrix、claim ledger、CSV 与中文教程；
- v0.1.0 发布后硬件验证补充证据包。

## 3. Release 与硬件补充边界

既有 v0.1.0 五文件本地 Release 保持不可变：

```text
release_status=LOCAL_RELEASE_READY
published=false
tag=null
hardware_validated=false
```

真实硬件结果记录在独立补充证据包中：

```text
docs/releases/v0.1.0_hardware_validation_supplement/
POST_RELEASE_G6_HARDWARE_SUPPLEMENT=PASS
```

这不表示原 `release_index.json` 已被回写，不表示创建了新版本、Git tag 或 GitHub Release。

## 4. 冻结证据基线

| 范围 | 身份/结果 |
|---|---|
| v0.1.0 产品源码 | `2a0c961bb9677fb3cc54b1e57be88a96cd9db82b` |
| T06 Release 工具 | `db309472b76d0f64d30d8d001db4cf40ace8380a` |
| G6-T02 正式候选 | `b35a01e25c95fb427fbfa165c4e7f3b030f73b8d` |
| G6-T03 正式候选 | `e873b5e31a7dde5990860cf641ee59241e5ef910` |
| G6-T04 正式候选 | `c5ddc365ae46c920a78d71ef67ee13fe9de39651` |
| G6/T05 文档固化 | `18e0af7c57e916af641e1e868995cb1a4855a631` |
| G6 最终状态 | `T00～T05=PASS`、`G6=PASS` |
| 故障恢复 | `17/17 PASS`、`failures=[]` |
| 正式硬件长稳 | `28802.252 s`、三从站成功率均 `1.000` |
| 原始证据保留 | 见 `docs/evidence_retention_manifest.md` |

## 5. 当前台架运行状态

冻结准备检查时，树莓派上的网关服务和 Mosquitto 均保持 `enabled/active`，网关
`NRestarts=0`。本项目选择保留可演示运行状态，不在冻结过程中停止服务、拆线或删除证据。

该状态属于检查时快照，设备断电或重启后可以变化；恢复与排障方法以 `docs/runbook.md` 为准。

## 6. 已知限制

- 当前验证是 Raspberry Pi 4B、桌面短线 RS485 和当前三从站组合；
- 没有计量基准，不声明温湿度测量精度认证；
- 没有覆盖长距离、EMC、浪涌、隔离等级或破坏性电气测试；
- CAN 只引用项目五历史证据，不是项目三 G6 通过门；
- 单次八小时长稳不能外推为工业级、生产可用或无限期可靠；
- 没有 GitHub Actions、Git tag、GitHub Release 或公开发布。

## 7. 维护与解冻规则

冻结后允许在 `MAINTENANCE_ONLY` 状态下进行：

- 文档错别字、失效链接和公共安全修订；
- 已有证据的备份、迁移和校验；
- 不改变能力声明的简历/作品集投影；
- 明确不改变产品行为的维护性说明。

以下变化必须先提出解冻原因、影响范围、测试计划和新冻结基线：

- 修改 C++、Python runner、CMake、依赖版本或正式配置；
- 改变串口合同、寄存器映射、MQTT 合同或 systemd 行为；
- 更换关键硬件、总线拓扑或扩大硬件能力声明；
- 重打二进制、创建新版本、tag、GitHub Release 或公开发布；
- 把 CAN、计量精度、长距离或工业现场能力纳入项目三声明。

若只修改文档和证据索引，不要求重复八小时测试。只有产品行为、正式配置或硬件能力边界发生
相关变化时，才按影响选择定向回归；是否重跑长稳必须由新变更风险决定，不能机械重复。

## 8. 冻结提交验收门

- 补充证据包两个 JSON 可解析；
- 包内 `SHA256SUMS` 全部有效；
- claim YAML/CSV 数量、ID、状态和来源一致；
- Markdown 相对链接和公共安全扫描通过；
- `git diff --check` 通过；
- 临时构建、补丁、预览、`.pyc` 和 `__pycache__` 未进入提交范围；
- 用户完成冻结提交后工作区干净。

本轮没有执行 Git 写操作。未提交工作区副本不构成冻结基线；本文档进入用户提交后，以上生效
规则自动满足，不需要再修改文档记录提交哈希。
