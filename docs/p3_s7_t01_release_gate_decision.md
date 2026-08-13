# P3-S7-T01：Release blocker 与硬件启动门决策记录候选稿

> 文档状态：已审核确认，P3-S7-T01 完成  
> 审查日期：2026-08-13  
> 审查仓库：`/home/iot-gw/projects/industrial_iot_gateway`  
> 审查候选提交：`4f207ebc9df354274ace1d87618efe47f1fe33fd`  
> S6 运行证据提交：`ecb4cacdf56e187940b341d647ff1529ca2f0b0c`  
> 软件路由：`ROUTE_TO_T02_MINIMAL_FIX`  
> 硬件路由：`WAITING_FOR_HARDWARE`

## 1. 最终判定

P3-S7-T01 的只读审查已经完成，但当前候选提交不能直接进入稳定 Release 冻结：

- 项目所有者已经批准 MIT License，但仓库尚无 `LICENSE`，README 仍写着许可证未确定；
- S7 冻结命令要求的 `tools/demo.py --profile pty-mqtt` 尚不存在；
- 第三方依赖清单需要按已安装包、实际链接方式和 Release 包边界修正并形成 NOTICE；
- S6 的 8 小时运行时证据可有条件继承，但安装、systemd 和候选包证据没有严格绑定当前提交；
- 当前 G5 只有 WSL2 开发证据，而冻结验收协议要求稳定 Release 在原生 Linux/systemd 环境执行；
- 硬件仍只有采购/待到货记录，没有到货验收、寄存器合同、电气安全和可控从站证据。

因此：

1. 下一步先执行 **P3-S7-T02 软件 blocker 最小修复**；
2. T02 关闭代码与文档 blocker 后，进入 **P3-S7-T04 稳定 Release 与复现演练**；
3. 当前不得启动 P3-S7-T03；硬件条件在 T04 冻结前仍不具备时，将 G6 从
   `WAITING_FOR_HARDWARE` 转为 `NOT_RUN_OPTIONAL`；
4. 不得把 PTY、WSL2、采购记录或一帧收发写成树莓派、ARM64、真实 RS485/CAN 硬件 PASS。

## 2. 已批准的 Release 边界

用户已于 2026-08-13 明确批准：

- 项目自有代码采用 **MIT License**；
- MIT 版权行采用 `Copyright (c) 2026 fengchechecheche`；
- `v0.1.0` 限定为 **Linux x86_64 软件 MVP**；
- 不宣称已经验证树莓派、ARM64、真实 USB-RS485、STM32 或 CAN 硬件。

推荐冻结表述：

> Industrial-IOT-Gateway v0.1.0 是 Linux x86_64 软件 MVP。项目已在冻结软件环境中完成
> PTY、MQTT、故障注入与 8 小时长稳验证；稳定 Release 仍须在最终提交上完成构建、演示和
> 原生 Linux/systemd 复现。树莓派、ARM64、真实 RS485/CAN 和硬件台架不在已验证范围内。

## 3. 证据输入与完整性检查

### 3.1 提交关系

当前候选提交相对 S6 证据提交只增加一个已提交版本：

```text
4f207eb [018] 第十八次提交，完成 P3-S6 八小时长稳与 systemd 验收
```

版本差异包含 S6 报告、runbook、systemd unit、安装规则、systemd 合同测试和质量摘要；
`src/` 与 `include/` 的变更数为 0。

### 3.2 S6 机器摘要解析

`artifacts/software_mvp/s6/quality_gate_summary.json` 可以解析，记录为：

| 字段 | 值 |
|---|---|
| `status` | `PASS` |
| `source_revision` | `ecb4cacdf56e187940b341d647ff1529ca2f0b0c` |
| 长稳时长 | `28800.055 s` |
| oracle | `212/212 PASS` |
| CTest | Release/Debug/ASan/TSan 均 `182/182 PASS` |
| systemd 环境 | `WSL2 Ubuntu 24.04 x86_64 systemd 255` |
| ARM64 验证 | `false` |
| 硬件验证 | `false` |

摘要、S6 报告和 README 对 ARM64/硬件的否定边界一致。

### 3.3 关键 Git blob

| 文件 | 当前提交 blob |
|---|---|
| `README.md` | `91707312885cc2d4d34908910c1d12fc965075e7` |
| `CMakeLists.txt` | `78bb0307d063f5cbfc28ec9f992e77b088b34fad` |
| `docs/p3_s6_validation.md` | `c9b92031ba419ef5c52959773d9ecbabc02231e0` |
| `docs/runbook.md` | `7841dd07a648bb949a1941f1acd5f829f480ef87` |
| `docs/third_party_inventory.md` | `9a90913e474fd355d3cdea3edcc86285b3221b72` |
| `docs/hardware_selection.md` | `7e69a9afa1a45fe467815e485ada662541dfee9d` |
| `artifacts/software_mvp/s6/quality_gate_summary.json` | `26cbe74cb4bb179bdc9fd0afd069996ac93aee33` |
| `packaging/systemd/industrial_iot_gateway.service` | `14141c66a7c26a207b898ad20c30011818c29d89` |
| `packaging/systemd/gateway.env.example` | `395d98ab0a845aa787b02de8da02fd5bdb6b12b9` |

## 4. Release blocker 清单

### S7-RB-01：MIT 决策尚未固化到仓库

| 属性 | 判定 |
|---|---|
| 等级 | P0 |
| 状态 | `OPEN` |
| 证据 | 当前提交无 `LICENSE`/`LICENSE.md`；README 仍写“尚未最终确定” |
| 影响 | 不能形成许可证清楚且可公开分发的稳定开源 Release |
| 处理工作块 | T02 |
| 最小修复 | 添加标准 MIT `LICENSE`；使用项目所有者确认的版权年份/主体；同步 README |
| 退出条件 | Release 源码树、README 和包内许可证一致；不存在“候选许可证”旧表述 |
| 复验 | Git 提交树存在文件；许可证文本与 OSI MIT 标准文本一致；包内包含许可证 |

说明：用户已经关闭“选择哪一种许可证”和“使用哪一条版权行”的决策，不等于仓库固化工作
已经完成。T02 必须按已确认文本添加标准 MIT `LICENSE`，不得改写版权主体或公开用户未授权的
真实姓名。

### S7-RB-02：冻结的一键 PTY + MQTT 演示入口缺失

| 属性 | 判定 |
|---|---|
| 等级 | P1 |
| 状态 | `OPEN` |
| 证据 | 当前提交树不存在 `tools/demo.py`；S7 计划明确调用 `python3 tools/demo.py --profile pty-mqtt` |
| 影响 | 接收者不能按冻结命令快速复现核心链路 |
| 处理工作块 | T02 |
| 最小修复 | 先写命令/退出码/清理合同测试，再实现 `pty-mqtt` 最小 profile |
| 退出条件 | 单命令完成前置检查、构建或定位产物、broker/PTY 启动、验证和进程回收 |
| 复验 | 正常路径返回 0；缺依赖/端口冲突/超时返回非 0；结束后无残留子进程 |

演示脚本不得复用 8 小时 runner 冒充快速演示，也不得隐藏 MQTT 或测试失败。

### S7-RB-03：第三方许可证声明未按发布边界闭环

| 属性 | 判定 |
|---|---|
| 等级 | P1 |
| 状态 | `OPEN` |
| 证据 | `docs/third_party_inventory.md` 有清单，但无稳定 Release 的 `THIRD_PARTY_NOTICES.md` |
| 影响 | 二进制包的第三方版权、许可证和取得源码/许可证的位置不够清楚 |
| 处理工作块 | T02 |
| 最小修复 | 修正依赖许可证名称；新增 NOTICE；把测试工具和运行时/编译入二进制的依赖分层 |
| 退出条件 | NOTICE 与固定包版本、链接/打包边界一致，并进入 Release 包 |
| 复验 | 对照 Debian copyright 和对应版本上游 LICENSE/NOTICE；检查包清单 |

本次核验发现的具体修订点：

- Ubuntu 包中的 yaml-cpp `0.8.0+dfsg-6build1` copyright 标为 `X11`，内容属于常见
  MIT/Expat 类文本；正式清单应记录包内标识，不能只写模糊的“MIT”；
- Ubuntu 的 Paho C++ `1.2.0-2` 与 Paho C `1.3.13-1build2` 包 copyright 标为
  `EPL-2.0`；如果 NOTICE 选择引用上游双许可选项，必须明确选择依据，不能把 Debian 包
  已记录的许可证直接改写成双许可；
- nlohmann/json `3.11.3-1` 是头文件依赖，其代码会编译进入 MQTT 目标，应保留 MIT/Expat
  版权与许可声明；
- GoogleTest 和 Mosquitto 当前仅用于测试，不进入网关 Release 运行包，但仍保留在开发依赖清单；
- yaml-cpp、Paho C++/C 和 glibc 为系统包/动态运行时依赖时，Release 文档应明确要求通过
  包管理器安装，不把它们写成仓库内置源码。

本记录只做工程发布清单审查，不构成法律意见。

### S7-RB-04：S6 systemd/安装证据未严格绑定当前提交

| 属性 | 判定 |
|---|---|
| 等级 | P1 |
| 状态 | `OPEN` |
| 证据 | 质量摘要绑定 `ecb4cac…`，但 CMake 安装规则、unit、环境模板和合同测试到 `4f207eb…` 才进入提交树 |
| 影响 | 无法由证据提交本身完整重建报告所称的 systemd/安装产物 |
| 处理工作块 | T04 |
| 最小修复 | 在最终候选提交上从清洁目录重跑安装暂存、systemd 合同、场景矩阵并生成新摘要/哈希 |
| 退出条件 | 报告、JSON、二进制、unit、环境模板和提交 ID 指向同一最终候选 |
| 复验 | 从最终提交重新构建；校验包 SHA-256；按 manifest 复跑命令 |

这不推翻 S6 首轮工程测试结果，但阻止把它直接当作最终 Release 的严格版本化证据。

### S7-RB-05：稳定 Release 的原生 Linux/systemd G5 证据缺失

| 属性 | 判定 |
|---|---|
| 等级 | P1 |
| 状态 | `OPEN` |
| 证据 | 当前 G5 环境为 WSL2；冻结的 `acceptance_protocol.md` 明确 WSL2 只能用于开发，稳定 Release 的 G5 必须在目标 Linux/systemd 原生执行 |
| 影响 | 当前证据不能满足稳定 `v0.1.0` 的 G5 门 |
| 处理工作块 | T04 |
| 最小修复 | 在干净的原生 Linux x86_64/systemd 环境复跑 G5 场景并绑定最终提交 |
| 退出条件 | 原生环境身份、内核/systemd 版本、命令、退出码、journal 摘要和校验和完整 |
| 复验 | 静态校验、非 root、SIGTERM、异常重启、错误配置、串口缺失、broker 不可达和重复启停全部 PASS |

如果无法提供原生 Linux x86_64 环境，则不能发布“稳定 Linux x86_64 v0.1.0”；只能继续保留
WSL2 开发候选/预览表述，或由用户重新批准更窄的版本定义。

### S7-RB-06：最终提交的清洁构建与 Release 演练尚未执行

| 属性 | 判定 |
|---|---|
| 等级 | P1 |
| 状态 | `OPEN` |
| 证据 | 当前只有 S6 RC 摘要；尚无包含 T02 修复的最终提交与 `v0.1.0` 正式 bundle |
| 影响 | 不能证明最终许可证、NOTICE、演示入口、安装文件和二进制可以共同复现 |
| 处理工作块 | T04 |
| 最小修复 | 清洁 Release/Debug/ASan/TSan、PTY、MQTT、demo、安装、systemd 与包清单复验 |
| 退出条件 | `artifacts/releases/v0.1.0/` 的 manifest、SHA-256、报告和 bundle 一致 |
| 复验 | 按公开命令在清洁环境演练一次，任何强制步骤失败均返回非 0 |

## 5. 已关闭或降级的事项

### S7-RB-07：项目许可证选择

- 状态：`CLOSED_DECISION`；
- 结论：MIT License；
- 注意：只关闭选择决策，文件固化仍由 S7-RB-01 追踪。

### S7-RB-08：ARM64/树莓派/真实硬件是否阻止 v0.1.0

- 状态：`CLOSED_BY_SCOPE`；
- 结论：不阻止 Linux x86_64 软件 MVP；
- 要求：README、Release notes、包 manifest 和项目介绍必须继续明确未验证边界。

## 6. 证据继承判定

| 证据 | 能否继承 | 结论 |
|---|---|---|
| G1 核心编译/单测历史结果 | 有条件继承 | 没有 `src/`/`include/` 变化；最终提交仍须在 T04 重跑 |
| G2 PTY 链路 | 有条件继承 | 运行时未变；T02 demo 必须新增独立复验 |
| G3 MQTT/故障矩阵 | 有条件继承 | MQTT 运行时未变；最终 Release 仍须做代表性断线恢复 |
| G4 8 小时长稳 | 可以继承 | 当前提交未修改运行时；T02 若只改许可证/NOTICE/独立 demo，无需重跑 8 小时 |
| G4 8 小时长稳 | 条件失效 | T02 若改串口、调度、队列、MQTT、质量或关闭逻辑，必须重新评估并可能重跑 |
| G5 WSL2 systemd | 仅作开发证据 | 版本绑定和原生环境两个条件均未满足稳定 Release 门 |
| S6 RC bundle | 不可直接转正 | 最终包必须包含 T02 修复并重新生成 manifest/SHA-256 |

## 7. 硬件启动门判定

| 门项 | 状态 | 证据与原因 |
|---|---|---|
| HG-01 软件 MVP 历史 G1～G5 输入存在 | `PASS_WITH_LIMITS` | S6 软件证据存在，但 G5 稳定版边界仍需 T04 关闭 |
| HG-02 核心物料到货并完成型号/外观检查 | `WAITING` | 仅有“已采购、待到货验收”记录 |
| HG-03 USB-RS485 驱动、自动方向、A/B/GND、终端和隔离明确 | `WAITING` | CH340 设备尚未到货验证，隔离能力不得宣称 |
| HG-04 TAS-WS-R00020 寄存器合同冻结 | `FAIL` | 完整手册、地址、串口参数、缩放和功能码尚未冻结 |
| HG-05 电气与供电安全 | `FAIL` | 开放式 12 V 市电电源在外壳、保险、保护接地和端子防护前禁止上电 |
| HG-06 至少一个可控寄存器从站 | `FAIL` | 尚无实物读/写/掉电恢复证据 |
| HG-07 剩余主动工时不少于 3.5 小时 | `NOT_EVALUATED` | 只在硬件条件全部具备时评估 |
| HG-08 树莓派目标可用 | `NOT_REQUIRED_FOR_V0.1.0` | 已批准的软件版本不声明树莓派验证 |

硬件门结论：`WAITING_FOR_HARDWARE`。HG-04、HG-05、HG-06 任一项都足以阻止 T03；当前不得
给开放式市电端子上电。如果稳定 Release 冻结时仍未全部具备，G6 记录为
`NOT_RUN_OPTIONAL`，不影响软件 MVP，但必须公开列为未验证能力。

## 8. T02 最小修复范围

T02 应严格限制为：

1. 添加经批准的 MIT `LICENSE` 并同步 README；
2. 修正 `docs/third_party_inventory.md`，新增 `THIRD_PARTY_NOTICES.md`，并让安装/打包包含必要声明；
3. 先写测试，再实现 `tools/demo.py --profile pty-mqtt`；
4. demo 使用回环匿名 broker、虚构 ID、明确超时和进程回收；
5. 增加 Release 包清单/文档合同测试，防止漏装 LICENSE、NOTICE、配置样例或 systemd 文件；
6. 不修改串口、调度、质量、新鲜度、MQTT 队列或关闭协议等运行时逻辑。

只要 T02 保持上述范围，8 小时 G4 可以继续继承；若实施中必须修改运行时，需停止并重新评估
长稳复验范围。

## 9. T04 强制复验范围

T02 关闭后，T04 至少完成：

- 最终提交上的清洁 Release 与 Debug 构建；
- ASan/UBSan、TSan、clang-tidy、clang-format、CTest；
- MQTT OFF 原有 PTY 路径；
- MQTT ON 的连接、断线、重连和串口继续采集；
- `python3 tools/demo.py --profile pty-mqtt` 的公开复现演练；
- CMake 安装暂存与包清单；
- 原生 Linux x86_64/systemd 的 G5 场景；
- LICENSE、THIRD_PARTY_NOTICES、README、runbook、配置和 systemd 文件进入包；
- `artifacts/releases/v0.1.0/` manifest、SHA-256、失败清单和验收摘要；
- G6 的最终实际状态，若未执行则为 `NOT_RUN_OPTIONAL`。

## 10. T01 验收对照

| 验收项 | 结果 |
|---|---|
| 记录候选提交与 S6 证据提交 | PASS |
| 解析并核对 S6 摘要 | PASS |
| blocker 均有 ID、等级、证据、路由和退出条件 | PASS |
| MIT 选择由用户明确批准 | PASS |
| 未把许可证选择写成已固化 | PASS |
| x86_64 与 ARM64/树莓派/硬件边界明确 | PASS |
| 硬件门逐项判定 | PASS |
| 未把硬件或 WSL2 证据扩大为稳定硬件/目标机 PASS | PASS |
| 未检查 Git 工作区未提交状态 | PASS |
| 未执行 `git add`、`git commit`、`git push` | PASS |
| 正式仓库维护版本 | PASS；同步到 `docs/` 与 `artifacts/software_mvp/s7/` |

用户已确认 `4f207ebc9df354274ace1d87618efe47f1fe33fd` 为 T01 审查候选提交，并批准 MIT
版权行 `Copyright (c) 2026 fengchechecheche`。正式维护版本同步到
`docs/p3_s7_t01_release_gate_decision.md` 和 `artifacts/software_mvp/s7/release_gate_summary.json`。

## 11. 许可证核验来源

访问日期均为 2026-08-13：

- [Open Source Initiative：MIT License](https://opensource.org/license/mit)
- [Eclipse Paho MQTT C++](https://github.com/eclipse-paho/paho.mqtt.cpp)
- [Eclipse Paho MQTT C](https://github.com/eclipse-paho/paho.mqtt.c)
- [yaml-cpp](https://github.com/jbeder/yaml-cpp)
- [nlohmann/json v3.11.3 LICENSE.MIT](https://github.com/nlohmann/json/blob/v3.11.3/LICENSE.MIT)
- [GoogleTest](https://github.com/google/googletest)
- [Eclipse Mosquitto](https://github.com/eclipse-mosquitto/mosquitto)
- WSL 已安装包：`/usr/share/doc/<package>/copyright`
