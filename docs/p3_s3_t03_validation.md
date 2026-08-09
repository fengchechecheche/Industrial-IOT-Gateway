# P3-S3-T03 超时、重试、退避和新鲜度验收记录

## 1. 验收结论

- 任务：`P3-S3-T03`（超时、重试、退避和新鲜度）。
- 执行日期：2026-08-09。
- 环境：`Ubuntu-24.04-Gateway`，x86_64，Linux 文件系统仓库。
- WSL 开发验收：**通过**。
- 提交后固定版本 Release 复验：**未执行**。该步骤需要用户另行授权 Git 操作并形成固定提交。

本任务在 T02 确定性轮询调度器之上实现了业务请求与单次发送尝试分离的生命周期、响应
超时、有限重试、有界指数退避、请求总截止时间、从站离线/探测/恢复状态机，以及独立的
寄存器新鲜度跟踪器。重试等待期间会释放单条 RTU 总线，其他从站可以继续得到服务。

本结论不表示跨线程队列、显式请求公平性、关闭协议、结构化日志或真实三个 PTY 的完整
网关主循环已经实现；这些仍分别属于 T04 和 T05。

## 2. 冻结可靠性参数

| 参数 | 默认值 | 语义 |
|---|---:|---|
| 单次响应超时 | 500 ms | 从本次发送时刻开始计算 |
| 最大尝试次数 | 3 | 包含首次发送，最多重试 2 次 |
| 首次退避 | 200 ms | 第一次失败后的等待时间 |
| 退避倍率 | 2.0 | 后续退避按倍数增长 |
| 退避上限 | 2000 ms | 所有退避不得超过该值 |
| 业务请求总截止时间 | 5000 ms | 从业务请求创建时刻开始计算 |
| 连续终态通信失败离线阈值 | 3 | 达到阈值后停止普通轮询 |
| 离线探测周期 | 5000 ms | 离线/探测状态下使用代表性轮询项探测 |
| 连续探测成功恢复阈值 | 2 | 达到阈值后恢复普通轮询 |

构造阶段会拒绝非正超时、尝试次数不在 `1..5`、非法退避参数、非有限或小于 1 的倍率、
无效截止时间，以及无法容纳最坏情况尝试和退避预算的截止时间配置。

## 3. 请求生命周期和边界

请求状态扩展为：

```text
QUEUED -> WAITING_RESPONSE -> RETRY_WAIT -> QUEUED
                         \-> COMPLETED / FAILED / CANCELLED
```

- 同一个业务请求的所有尝试保持相同 `request_id`，`attempt` 从 1 递增；
- 每次发送都有独立的响应截止点，取“发送时刻 + 500 ms”和业务总截止点的较早者；
- 未收到结果时，在响应截止点的精确边界触发 timeout；外部结果在边界时刻到达仍可被接受；
- 第 1、2 次可重试失败分别形成 200 ms、400 ms 退避；第 3 次失败终止请求；
- 退避起点若不早于业务总截止点，直接以 `deadline_exceeded` 终止；
- `RETRY_WAIT` 不占用总线，其他从站可以在等待窗口内派发；
- CRC 错误、截断帧、瞬态串口错误和响应超时可重试；
- 非法配置、广播不支持、主动取消和 deadline 不重试；
- Modbus 异常响应不重试，业务终态为 `COMPLETED`，但仍计为业务失败；它证明链路可通信并
  清零连续通信失败计数，不会把寄存器数据标记为 fresh。

保留了 T02 的 `complete_request()` 兼容入口，既有终态完成、统计和总线释放测试保持通过。

## 4. 离线、探测和恢复

普通请求只有在全部重试用尽或因通信导致 deadline 终止后，才累计一次“连续终态通信失败”。
成功响应和 Modbus 异常响应都证明通信存在并清零该计数。连续 3 次终态通信失败后：

1. 从站进入 `OFFLINE`；
2. 普通轮询暂停；
3. 5 秒后由该从站第一个冻结轮询项发起恢复探测；
4. 首次通信成功使状态进入 `PROBING`；
5. 再等待 5 秒并取得第二次连续探测成功后恢复 `ONLINE`；
6. 任一次探测失败都会把连续探测成功计数清零，并安排下一次探测。

从站健康统计记录离线转换、探测派发、探测成功、恢复转换和下一探测时刻。状态机只依赖调用
方传入的 `steady_clock::time_point`，测试不使用 `sleep()` 或墙上时钟。

## 5. 新鲜度语义

`FreshnessTracker` 为冻结寄存器表的 16 个轮询项维护独立质量状态：

- `NO_VALID_SAMPLE`：尚无有效历史值；
- `FRESH`：存在有效值且 `now - last_valid <= freshness`；
- `STALE`：存在保留值且严格超过 freshness，或已有历史值的轮询终态失败；
- `INVALID`：收到业务上无效的样本，不会伪装成 fresh；
- `OFFLINE`：从站离线时覆盖该从站所有寄存器的外部质量。

新鲜度边界采用严格大于：恰好达到 `freshness` 仍为 fresh，超过 1 ms 才转 stale。离线期间的
旧值不会恢复为 fresh；从站恢复在线后，曾经有效的历史值保持 stale，直到新的有效样本到达。
所有变更返回结构化质量转换，并标注 stale/offline 时旧值是否仍被保留。

## 6. 测试先行证据

先加入公共类型、21 个测试和 `gateway_reliability_policy_test` 目标，不加入实现源文件。三个测试
源文件成功编译，随后链接阶段按预期出现 `ReliabilityPolicy`、新增 `PollScheduler` 接口和
`FreshnessTracker` 的 undefined reference。加入最小实现后再修正一处枚举命名，定向目标
21/21 通过。

新增测试包括：

- 7 个可靠性策略测试：默认值、非法配置、分类、退避上限、尝试序列、deadline 和终态；
- 7 个调度可靠性测试：冻结字段、精确超时边界、退避期间跨从站服务、三次尝试、截止时间、
  离线/双探测恢复、异常响应和健康配置；
- 7 个新鲜度测试：配置、精确边界、失败转 stale、invalid、offline 覆盖、时间倒退和冻结
  三从站 16 项投影。

## 7. 质量门

四套配置均启用 `GATEWAY_BUILD_PTY_SLAVE=ON` 和项目代码警告视为错误，并在最终代码上执行：

| 配置 | clean-first 构建 | CTest | 其他结果 |
|---|---|---:|---|
| Debug | 通过 | 95/95 | 无编译警告 |
| Debug + ASan/UBSan | 通过 | 95/95 | 无 sanitizer 报告 |
| Release | 通过 | 95/95 | 完整测试通过 |
| Debug + clang-tidy | 通过 | 95/95 | 最终全量构建零告警 |

最终标签统计：

- `unit`：84/84，其中新增 T03 用例 21 个；
- `integration`：9/9；
- `pty`：2/2 聚合门。

10 个 T03 C++ 文件均通过 `clang-format-18 --dry-run --Werror`。clang-tidy 首轮指出可选值访问
证明和相邻可交换整数参数问题；最终通过显式状态检查、把恢复探测标记作为强语义参数传递，
并对既定公共接口加入一处带理由的窄范围抑制后，实现全量零告警。

依据仓库根 `AGENTS.md` 权限红线，本任务未执行 `git status`、用于检查工作区的 `git diff`、
暂存、提交或推送。文件正确性通过定向读取、编译、测试、静态检查、格式检查和 SHA256 固化
验证。

## 8. 交付文件

| 文件 | 用途 | SHA256 |
|---|---|---|
| `include/industrial_iot_gateway/scheduler/scheduled_request.hpp` | 请求状态、结果、尝试与统计类型 | `50b50044161b84b2307f2a134d0d336e502fc70d20604988af300a90dca1478a` |
| `include/industrial_iot_gateway/scheduler/reliability_policy.hpp` | 可靠性参数、决策和错误接口 | `2d7d3bfd3a4cc5b639668b781195cd9b73ab5885a4434b503f078d2803409446` |
| `src/scheduler/reliability_policy.cpp` | 超时、退避、重试和 deadline 决策 | `5161183aee7350a1ab82b5361977daabbf6fd631ddc0618d7c7ddae48045b59e` |
| `include/industrial_iot_gateway/scheduler/poll_scheduler.hpp` | 调度器生命周期和设备健康 API | `1ce0c8c01d94034833c25a6f7a85796eba213f428f3d6a23128df4deb7af51f0` |
| `src/scheduler/poll_scheduler.cpp` | 重试调度、离线探测和恢复实现 | `cb1c8a023dc229e018e57d308c4fd8333e7c22ceee4a4d729e9b8d305ba5829f` |
| `include/industrial_iot_gateway/quality/freshness_tracker.hpp` | 新鲜度质量公共接口 | `4f487cce155a2473abda5c5a3125a7696ab3f177365f62450bb562f84261eed6` |
| `src/quality/freshness_tracker.cpp` | 质量状态和边界实现 | `e7551d51d461c5fc0ef25c5e7d9a3bcec1a8c62e7182aef00aafebee23165d9b` |
| `tests/unit/reliability_policy_test.cpp` | 可靠性策略测试 | `fc34d3f0e6c9b6f1e3c7c414177440fee30249517cb4066523b6f8c6cb31da0c` |
| `tests/unit/poll_scheduler_reliability_test.cpp` | attempt、离线和恢复测试 | `d3a89d4026c8eabe8a14c63a1e5bf2fbe9ec947b4581ec37ea1639734eca2664` |
| `tests/unit/freshness_tracker_test.cpp` | 新鲜度质量测试 | `ec8a3b66bba61aa9f362600aac2954f4e5e90d629459921f82d1d0f5feefe36a` |

另修改根 `CMakeLists.txt` 和 `tests/CMakeLists.txt`，将可靠性策略和新鲜度实现加入
`gateway_core`，并注册 T03 单元测试目标。

## 9. 后续边界

T03 向后续任务移交确定性的可靠性决策、可释放总线的重试生命周期、从站健康状态和寄存器
质量投影。以下能力仍未实现，不能由本记录推定完成：

- `P3-S3-T04` 有界跨线程队列、显式请求公平性、关闭协议和结构化日志；
- `P3-S3-T05` scheduler、transport、codec/parser 与三个 PTY 的真实网关主循环集成；
- MQTT、真实 UART/USB-RS485 和 RS485 电气验证。
