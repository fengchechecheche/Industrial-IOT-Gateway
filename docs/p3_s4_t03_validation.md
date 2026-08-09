# P3-S4-T03 验收报告

> 工作块：P3-S4-T03——故障注入编排与结果 schema  
> 验证日期：2026-08-09  
> 仓库：`/home/iot-gw/projects/industrial_iot_gateway`  
> 文档状态：已审核固化并同步为独立仓库正式维护版本

## 1. 结论

**T03 工程实现与验收：PASS。**

已实现可重复执行的软件故障矩阵 runner，覆盖 F01–F15；绑定本次实现提交的正式执行为
15/15 PASS、未关闭失败 0，全部证据通过 SHA256 校验。Debug、ASan/UBSan、Release、
clang-tidy、TSan、clang-format 与完整 CTest 均通过。

**正式 G3 基线：PASS。**

正式运行 `20260809T155317Z_g3_b293e40_001` 明确记录 `exploratory=false`，并绑定完整提交 ID
`b293e402fe3b45e60de3c8f09512378c959f4bbd`。F01–F15 全部通过，`unclosed_failures=0`，
正式证据目录的 `SHA256SUMS` 完整性校验通过。

## 2. 交付物

### 2.1 Runner 与证据合同

- `tools/run_fault_matrix.py`
- `tools/fault_matrix/__init__.py`
- `tools/fault_matrix/models.py`
- `tools/fault_matrix/process_manager.py`
- `tools/fault_matrix/evidence.py`
- `tools/fault_matrix/runner.py`
- `tools/fault_matrix/README.md`
- `schemas/fault_evidence.schema.json`
- `tests/data/fault_profiles/software.json`
- `tests/fault/test_run_fault_matrix.py`

### 2.2 运行时与故障注入支持

- `GatewayRuntimeConfig` 增加请求队列与测量队列配置；
- `GatewayRuntimeStatistics` 增加 `measurement_enqueue_failures`；
- 非成功测量入队产生 `measurement_queue_rejected` 结构化事件；
- 非法请求/测量队列配置在线程启动前 fail-fast；
- PTY server 与 harness 支持运行时切换 `FaultPlan`；
- MQTT sink 的关闭截止时间由专用互斥量保护，修复 TSan 发现的竞态。

### 2.3 测试

- `tests/fault/fault_contract_test.cpp`
- `tests/integration/mqtt_slow_ack_integration_test.cpp`
- `tests/integration/gateway_runtime_pty_test.cpp` 的 F05、F06、F13 增强
- `tests/integration/mqtt_pty_runtime_integration_test.cpp` 的 F10 恢复指标
- `tests/support/pty_bus_harness.*` 与 `tools/pty_slave/pty_slave_support.*` 动态故障接口

### 2.4 教学与验收文档

- `p3_s4_t03_故障注入与可观测性.md`
- `p3_s4_t03_validation.md`

## 3. 测试先行与修复记录

1. Python runner 测试首次执行因 `tools.fault_matrix` 尚不存在而失败；完成最小模块后转绿。
2. 首版慢 PUBACK 场景使用 `{capacity=8, normal_limit=6}`，关键 stale/offline/status 事件空间
   不足，测试观察到 `critical_enqueue_failures=11`；改为容量 64、普通遥测上限 6，明确保留
   关键容量后通过。
3. F05 初版只覆盖默认异常码 0x02；参数化扩展为 0x01、0x02、0x03、0x04 四个子用例。
4. 首版 runner 把整个 CTest 进程耗时写成恢复时间；修正为只有 F06/F10/F11/F13 从测试输出
   `FAULT_RECOVERY_TIME_MS`，其他场景写 `null`。缺少必需指标时 runner 即使收到退出码 0 也
   判定失败。
5. CTest verbose 会给测试输出添加 `<test-number>:` 前缀；runner 首次未解析到恢复指标并正确
   产生四个失败记录，随后扩展解析规则并由单元测试覆盖带前缀形式。
6. TSan 首轮 175/176，F11 检出 `drain_deadline_` 在主线程写、MQTT worker 读之间的数据竞态；
   使用 `shutdown_mutex_` 保护并保证“先设置截止时间、后发布停止标志”，定向及全量复测通过。

## 4. Runner 自身验收

| 项目 | 证据 | 结果 |
|---|---|---|
| 必填 event schema | 缺字段拒绝、完整事件接受 | PASS |
| 场景超时 | SIGTERM、宽限后 SIGKILL、最终 wait | PASS |
| 进程组清理 | `start_new_session=True` + `killpg()` | PASS |
| 单场景失败不短路 | 后续场景仍执行并留证 | PASS |
| 失败退出码 | FAIL/ERROR 映射为非零 | PASS |
| 恢复指标 | 必需场景缺失指标即 FAIL | PASS |
| 证据完整性 | 生成并回读 `SHA256SUMS` | PASS |
| Git 权限边界 | 不查询工作区，不调用 add/commit/push | PASS |

`fault_runner_unit` 最终纳入 CTest，完整套件共 176 项。

## 5. F01–F15 结果

最终证据：

`artifacts/baseline/20260809T155317Z_g3_b293e40_001/`

| ID | 场景 | 结果 | 恢复时间 |
|---|---|---|---:|
| F01 | 从站静默 | PASS | 不适用 |
| F02 | 响应延迟 | PASS | 不适用 |
| F03 | 坏 CRC | PASS | 不适用 |
| F04 | 截断帧 | PASS | 不适用 |
| F05 | Modbus 0x01–0x04 异常响应 | PASS | 不适用 |
| F06 | 串口断开与恢复 | PASS | 20 ms |
| F07 | 请求队列满 | PASS | 不适用 |
| F08 | 测量队列满 | PASS | 不适用 |
| F09 | 发布队列满 | PASS | 不适用 |
| F10 | broker 断开与恢复 | PASS | 1426 ms |
| F11 | PUBACK 延迟 | PASS | 20 ms |
| F12 | invalid 原始值 | PASS | 不适用 |
| F13 | stale/offline 与恢复 | PASS | 30 ms |
| F14 | SIGTERM 有界关闭 | PASS | 不适用 |
| F15 | 错误配置 fail-fast | PASS | 不适用 |

矩阵总耗时 7522 ms；`scenario_count=15`、`unclosed_failures=0`。恢复时间是该次运行的观察值，
不是生产环境 SLA，也不是统计分布上界。

## 6. 证据结构与追踪性

根目录包含：

- `manifest.json`
- `config.yaml`
- `environment.json`
- `commands.txt`
- `events.jsonl`
- `summary.json`
- `failures.json`
- `run.log`
- `SHA256SUMS`

`scenarios/F01` 至 `scenarios/F15` 均包含：

- `config.yaml`
- `events.jsonl`
- `summary.json`
- `failures.json`

事件 ID 采用 `<run_id>:<scenario_id>:<sequence>`；场景摘要引用开始与完成事件 ID。最终执行
`sha256sum -c SHA256SUMS`，所有文件均为 `OK`。

## 7. 质量门

所有构建目录均位于 `/tmp`：

| 质量门 | 配置 | 最终结果 |
|---|---|---|
| Debug | MQTT ON、PTY、warnings-as-errors | PASS，176/176 |
| ASan/UBSan | MQTT ON、PTY | PASS，176/176 |
| Release | MQTT ON、PTY | PASS，176/176 |
| clang-tidy 18 | Clang 18、MQTT ON、全目标 | PASS；T03 新增诊断已清理 |
| TSan | Clang 18、WSL2 `setarch -R` | PASS，176/176 |
| clang-format 18 | 全仓 `--dry-run --Werror` | PASS |
| Python | `py_compile` | PASS |
| JSON | `python3 -m json.tool` | PASS |

clang-tidy 仍会报告仓库既有的 `runtime_config.cpp` 异常逃逸提示，以及既有测试中的 optional 和
相邻参数提示；T03 新增代码的 move、相邻参数等诊断已清理。本门的成功定义与项目现有配置一致：
clang-tidy 完成全目标构建，并未把所有 clang-tidy warning 全局提升为 error。

## 8. 验收条目

| 验收要求 | 结果 | 说明 |
|---|---|---|
| runner 可重复执行 F01–F15 | PASS | software profile 固定顺序与场景映射 |
| broker 断线期间串口继续 | PASS | F10 直接检查请求成功继续增长 |
| 慢消费者/发布背压 | PASS | F11 暂停 PUBACK 方向、轮询继续、恢复后出现新 publish success |
| 请求/测量/发布队列有界 | PASS | F07–F09 |
| stale/offline/invalid 遵守合同 | PASS | F12、F13 |
| 关闭有界 | PASS | F14 |
| 结果文件相互追踪 | PASS | run/scenario/event ID 与 evidence IDs |
| runner 非零表示失败 | PASS | 单元测试覆盖失败聚合 |
| 不出现死锁或竞态 | PASS | TSan 176/176；发现并修复一处真实关闭竞态 |
| 正式证据绑定提交 | PASS | `exploratory=false`；提交 `b293e402fe3b45e60de3c8f09512378c959f4bbd` |

## 9. 能力与结论边界

本工作块证明的是 WSL2 本地软件链路：PTY、串口封装、调度、质量状态、MQTT 回环 broker、队列和
关闭协议在固定依赖版本下通过测试。它不证明真实 RS485 物理层、STM32 从站、生产 TLS/认证、
跨主机网络或长期随机组合故障。

`artifacts/baseline/` 受 `.gitignore` 保护。本报告记录了证据位置，但没有执行 Git 状态检查、
`git add`、`git commit` 或 `git push`，也没有擅自改变证据的提交边界。

## 10. 后续动作

1. T02/T03 教程与验收报告已审核并同步为独立仓库正式维护版本；
2. 正式 G3 已使用提交 `b293e402fe3b45e60de3c8f09512378c959f4bbd` 执行非 exploratory
   runner，结果为 PASS；
3. 进入 P3-S4-T04 时，仅在硬件和电气安全条件满足后执行台架，否则记录 `NOT_RUN`。
