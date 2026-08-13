# P3-S7-T02：软件 blocker 最小修复验收报告

> 文档状态：正式维护版，已于 2026-08-13 审核通过  
> 执行日期：2026-08-13  
> T02 基线提交：`3b8800aafbd788be72fffa55d52ad0bdc500559b`  
> 验收环境：WSL2 Ubuntu 24.04 Linux x86_64  
> 当前结论：**T02 范围 PASS，软件路由转为 `READY_FOR_T04`；G5/G6 未因此通过**

## 1. 总结

P3-S7-T02 已按“合同 RED → 最小实现 → PTY+MQTT 演示 → 质量门 → 教程与报告 → 临时文件
清理”的顺序执行。

T01 路由给 T02 的三个 blocker 已关闭：

- `S7-RB-01`：MIT `LICENSE`、版权行、README 和安装清单已经一致；
- `S7-RB-02`：`python3 tools/demo.py --profile pty-mqtt` 已实际执行并 PASS；
- `S7-RB-03`：第三方 NOTICE 和 inventory 已按 Ubuntu 固定包及交付层级闭环。

本工作块没有修改 `src/` 或 `include/` 中的运行时逻辑，因此 S6 的 8 小时 G4 证据仍满足 T01
定义的继承条件。T01 的 `S7-RB-04`～`S7-RB-06` 仍为 T04 的 Release blocker。

## 2. 实施范围

### 2.1 新增文件

- `LICENSE`；
- `THIRD_PARTY_NOTICES.md`；
- `tools/demo.py`；
- `tests/release/test_demo.py`；
- `tests/release/test_release_contract.py`。

### 2.2 修改文件

- `README.md`；
- `CMakeLists.txt`；
- `tests/CMakeLists.txt`；
- `docs/third_party_inventory.md`；
- `tools/fault_matrix/process_manager.py`。

`process_manager.py` 的唯一行为补强是：外部命令在 Ctrl+C 期间也先终止本次新建的进程组，再把
`KeyboardInterrupt` 继续抛给 demo 主程序；没有修改 fault/soak 的正常运行语义。

## 3. 测试先行证据

首次运行新增合同得到预期 RED：

- `LICENSE` 不存在；
- README 仍写许可证未确定；
- `THIRD_PARTY_NOTICES.md` 不存在；
- inventory 缺少固定 Ubuntu 包许可证边界；
- CMake 未安装 LICENSE、NOTICE、文档和配置；
- `tools.demo` 无法导入。

这证明新增测试先捕获了 T01 所列真实 blocker，而不是在实现后编写只能通过的装饰性测试。

最小实现后，最终合同单元测试为：

```text
Ran 13 tests
OK
```

覆盖 profile、操作系统、x86_64 架构、依赖、build 路径约束、jobs、命令数组、逐步失败、超时、
Ctrl+C 进程组清理、禁止全局 pkill、LICENSE、NOTICE、README、inventory 和 CMake install。

## 4. 一键 PTY+MQTT 演示

公开命令：

```bash
python3 tools/demo.py --profile pty-mqtt
```

冷启动正式执行结果：

| 步骤 | 结果 | 耗时 |
|---|---|---:|
| CMake configure | PASS | 6726 ms |
| 目标构建 | PASS | 17635 ms |
| MQTT+PTY 集成测试 | 1/1 PASS | 2917 ms |
| 总命令 | PASS | 28.4 s |

机器摘要：

```text
DEMO_PROFILE=pty-mqtt
DEMO_SCOPE=Linux_x86_64_PTY_MQTT_software_only
HARDWARE_VALIDATED=false
DEMO_RESULT=PASS
```

增量复验约 5 秒再次 PASS。真实测试为：

```text
MqttPtyRuntimeIntegrationTest.SerialPollingContinuesWhileBrokerIsUnavailable
```

它验证 broker 停止期间串口轮询继续、broker 恢复后 MQTT 重连和有界停止。它不等于 8 小时
长稳、原生 systemd、ARM64 或硬件证据。

## 5. 质量门结果

| 质量门 | 配置 | 结果 |
|---|---|---|
| Debug | MQTT ON、PTY ON、warnings-as-errors | 184/184 PASS |
| ASan/UBSan | MQTT ON、PTY ON、warnings-as-errors | 184/184 PASS |
| Release | MQTT ON、PTY ON、warnings-as-errors | 184/184 PASS |
| TSan | MQTT ON、PTY ON、WSL2 `setarch -R` | 184/184 PASS |
| MQTT OFF | Release、PTY ON、warnings-as-errors | 172/172 PASS |
| clang-tidy 18.1.3 | 全 C++ 目标 | 构建 PASS；仅有既有 optional/异常逃逸类提示 |
| clang-format 18 | `include/`、`src/`、`tests/`、`tools/` C++ 文件 | PASS |
| Python `py_compile` | 24 个 Python 文件 | PASS |
| T02 合同测试 | 13 个 unittest | PASS |
| 公开 demo | 冷启动和增量 | PASS |
| 安装暂存 | 二进制、systemd、配置、LICENSE、NOTICE、README、runbook | PASS |

四个 MQTT ON 全量配置均包含 184 项测试，比 S6 的 182 项多出：

- `release_contract_unit`；
- `demo_cli_unit`。

最终 Python 补强后，又在 Debug、ASan、Release、TSan 构建目录分别复跑 `release` 标签，均为
2/2 PASS。

## 6. 安装暂存结果

CMake 安装树已验证以下文件存在，文档和配置与源文件逐字节一致：

```text
usr/local/bin/gateway_app
usr/local/lib/systemd/system/industrial_iot_gateway.service
usr/local/share/industrial_iot_gateway/systemd/gateway.env.example
usr/local/share/industrial_iot_gateway/config/register_map.yaml
usr/local/share/industrial_iot_gateway/config/pty_slave_scenarios.yaml
usr/local/share/doc/industrial_iot_gateway/LICENSE
usr/local/share/doc/industrial_iot_gateway/THIRD_PARTY_NOTICES.md
usr/local/share/doc/industrial_iot_gateway/README.md
usr/local/share/doc/industrial_iot_gateway/runbook.md
```

源码树 demo 与安装树职责不同：`tools/demo.py` 需要构建测试源码，不承诺从安装目录运行；T04
应为源码归档和安装归档分别生成 manifest。

## 7. Blocker 关闭情况

| Blocker | T02 结论 | 证据 |
|---|---|---|
| S7-RB-01 MIT 未固化 | PASS/CLOSED | LICENSE、README、Release 合同、安装暂存 |
| S7-RB-02 一键 demo 缺失 | PASS/CLOSED | 13 项单测、冷启动 28.4 秒、真实集成 1/1 PASS |
| S7-RB-03 第三方声明未闭环 | PASS/CLOSED | NOTICE、inventory、安装合同 |
| S7-RB-04 S6 systemd/安装证据未绑定最终提交 | OPEN/T04 | 必须在最终提交重跑 |
| S7-RB-05 原生 Linux/systemd G5 缺失 | OPEN/T04 | 当前仍只有 WSL2 开发环境 |
| S7-RB-06 最终清洁 Release/bundle 未完成 | OPEN/T04 | T02 不制作正式 v0.1.0 |

软件下一路由：`READY_FOR_T04`。

## 8. 许可证与依赖边界

- 项目自有代码：MIT License；
- 版权行：`Copyright (c) 2026 fengchechecheche`；
- yaml-cpp Ubuntu 包记录：X11（MIT-style）；
- Paho C++/C Ubuntu 包记录：EPL-2.0；
- nlohmann/json 头文件实现编译进入 MQTT 目标，NOTICE 保留 MIT/Expat 文本；
- GoogleTest 和 Mosquitto 是测试专用依赖；
- 当前 Release 安装树不复制第三方共享库。

若 T04 打包第三方 `.so`，必须重新复核实际分发义务。本结论是工程盘点，不构成法律意见。

## 9. 能力与证据边界

本报告可以支持以下陈述：

> 已为 Linux x86_64 C++ 工业通信网关固化 MIT 与第三方依赖边界，实现一键 PTY+MQTT
> 软件演示，并通过 Debug、Release、ASan/UBSan、TSan、MQTT OFF 和安装清单合同。

本报告不能支持以下陈述：

- 已发布稳定 `v0.1.0`；
- 原生 Linux/systemd G5 已通过；
- 树莓派或 ARM64 已验证；
- USB-RS485、STM32、CAN 或真实传感器已联调；
- 生产 MQTT TLS/凭据已完成；
- 软件已经达到“工业级认证”或生产部署结论。

G6 当前仍为 `WAITING_FOR_HARDWARE`。如果 T04 冻结稳定软件 Release 时仍未执行，应改记为
`NOT_RUN_OPTIONAL`，不得写成 PASS。

## 10. 临时文件清理

本任务创建的以下临时路径均已在报告与教程生成后删除：

- `build/demo-pty-mqtt`；
- `build/t02-debug`；
- `build/t02-asan`；
- `build/t02-release`；
- `build/t02-tsan`；
- `build/t02-tidy`；
- `build/t02-mqtt-off`；
- `build/t02-install`；
- 本任务 Windows `.codex_tmp_p3_s7_t02` 编辑副本；
- 本任务产生的 `__pycache__` 和 `.pyc`。

清理只针对本任务创建并逐项解析确认的路径，不删除用户其他构建目录。清理后复核结果为：
上述 8 个构建目录、7 个 `__pycache__` 目录和 Windows 编辑副本均不存在。

## 11. 结论

P3-S7-T02 在冻结范围内 **PASS**：RB-01～RB-03 已关闭，运行时代码未修改，8 小时 G4 继承
条件保持成立。下一步不是直接宣称稳定 Release，而是审核并固化本教程和验收报告，然后制定
P3-S7-T04，关闭最终提交绑定、原生 Linux/systemd G5 和正式 bundle 三个剩余 blocker。
