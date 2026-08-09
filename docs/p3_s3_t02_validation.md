# P3-S3-T02 请求调度与多从站轮询验收记录

## 1. 验收结论

- 任务：`P3-S3-T02`（请求调度与多从站轮询）。
- 执行日期：2026-08-09。
- 环境：`Ubuntu-24.04-Gateway`，x86_64，Linux 文件系统仓库。
- WSL 开发验收：**通过**。
- 提交后固定版本 Release 复验：**未执行**。该步骤需要用户另行授权 Git 操作并形成固定提交。

本任务实现了不依赖串口和线程的确定性轮询调度核心。调度器根据单调时钟、轮询周期、
到期时间和优先级选择请求，在单条 Modbus RTU 总线上强制最多一个在途请求；同时生成
进程内单调请求 ID、记录请求时序观察值，并维护三个从站的独立统计。

本结论不表示 response timeout、自动重试、退避、offline/recovery、freshness、有界跨线程
队列、真实三 PTY 网关主循环或 MQTT 已实现。

## 2. 冻结轮询基线

测试夹具逐项投影 `config/examples/register_map.yaml` 中三个启用从站的全部轮询定义：

| 从站 | 设备 | 启用轮询项 | 周期集合 |
|---|---|---:|---|
| 1 | `environment_sensor` | 5 | 500/1000/5000 ms |
| 2 | `motor_actuator` | 5 | 200/500/1000 ms |
| 3 | `fault_injection_device` | 6 | 1000 ms |
| 合计 | 3 个从站 | 16 | 200..5000 ms |

每个寄存器定义对应一个 `PollJob`；`float32` 和 `uint32` 等多寄存器值使用自身的
`register_count=2`。T02 不跨寄存器合并相邻地址，避免在尚未实现数据映射层时改变冻结语义。

## 3. 公共接口与职责边界

公共接口提供：

- `PollJob`：轮询任务 ID、`protocol::ReadRequest`、周期和优先级；
- `ScheduledRequest`：请求 ID、任务 ID、请求类型、协议请求和创建/入队时间；
- `RequestObservation`：发送、首字节、完成时间以及终态成功标志；
- `SlavePollStatistics`：派发、完成、失败、合并周期、最大迟到和最近时序；
- `PollScheduler`：计划校验、到期派发、终态完成、下一到期时间和从站统计。

调度核心复用 `protocol::Request`，没有复制功能码或 Modbus 编解码合同。它只接收调用方显式
传入的 `std::chrono::steady_clock::time_point`，不读取墙上时钟、不调用 `sleep()`、不打开
串口、不持有 fd，也不创建线程。

## 4. 调度语义

### 4.1 单总线单在途

`dispatch_next()` 返回请求时立即占用唯一在途槽位。在收到与当前 `request_id` 和
`poll_job_id` 同时匹配的终态观察值前，后续派发返回空值。错误 ID、错误任务 ID和非法时间
顺序不会释放槽位或修改统计。

成功或失败终态都会释放总线，确保已经得到终态的故障从站不会永久阻止其他从站。静默从站
没有终态时如何释放总线属于 T03 的 response timeout/deadline，不由 T02 假定完成。

### 4.2 到期、优先级与跨从站轮转

候选请求首先按最早 `next_due_at` 选择；到期时间相同才比较优先级；到期时间和优先级均
相同时，按照上次服务从站后的循环距离轮转，最后用任务历史和 `poll_job_id` 保证确定性。

当前冻结寄存器表没有逐寄存器优先级字段，因此 16 个实际轮询任务均使用普通优先级。短周期
通过更频繁的到期时间表达，不被静默解释为更高优先级。显式读写与普通轮询的 8 次公平上限
将在 T04 接入显式请求队列时实现。

### 4.3 周期推进与防突发

派发后从原计划相位推进下一周期，而不是从实际派发时间重新计时。若调度晚了多个周期，
调度器只生成一个当前请求，并把已经错过的完整周期累计到 `poll_periods_coalesced`；不会集中
补发全部历史轮询，从而避免串口恢复后形成请求突发。

## 5. 输入校验与错误模型

构造阶段拒绝：

- 空轮询计划；
- 重复 `poll_job_id`；
- 从站地址不在 `1..247`；
- 非 `0x03/0x04` 的轮询功能码；
- 数量不在 `1..125`；
- 地址跨度越过 `65535`；
- 小于等于零的轮询周期。

运行阶段对无在途请求、请求 ID 不匹配、任务 ID 不匹配、时序倒退和请求 ID 耗尽返回结构化
`SchedulerErrorCategory`。发送/首字节字段可以在失败终态中缺失；一旦提供，必须满足：

```text
enqueued_at <= sent_at <= first_byte_at <= completed_at
```

## 6. 测试先行证据

先加入公共头文件、12 个单元测试和 CMake 测试目标，不加入实现源文件。测试源成功编译，
链接阶段按预期出现实现符号缺失，包括：

```text
undefined reference to `industrial_iot_gateway::scheduler::PollScheduler::PollScheduler(...)`
undefined reference to `industrial_iot_gateway::scheduler::PollScheduler::dispatch_next(...)`
undefined reference to `industrial_iot_gateway::scheduler::PollScheduler::complete_request(...)`
undefined reference to `industrial_iot_gateway::scheduler::PollScheduler::slave_statistics(...)`
```

加入最小实现后，新测试首次运行即为 12/12 通过。随后通过全量回归和 clang-tidy 反馈收紧
接口：去除无效的 trivially-copyable `std::move`，避免 `noexcept` 函数中的潜在抛异常
`std::get`，并把测试 optional 访问改为显式受检辅助函数。

## 7. 自动测试覆盖

新增 12 个单元测试覆盖：

- 空计划、重复任务 ID和非法协议字段；
- 单在途请求与单调递增 `request_id`；
- 错误完成事件不释放在途槽位；
- 同时到期任务在三个从站间轮转；
- 相同到期时间的优先级选择；
- 200/500/1000 ms 不同周期和禁止提前派发；
- 多周期迟到时合并历史周期且不突发补发；
- 三从站独立成功、失败和时序统计；
- 一个从站返回失败终态后继续服务下一从站；
- 冻结寄存器表三个从站、16 个轮询项完整进入有效计划。

## 8. 质量门

四套配置均启用 `GATEWAY_BUILD_PTY_SLAVE=ON` 和项目代码警告视为错误：

| 配置 | clean-first 构建 | CTest | 其他结果 |
|---|---|---:|---|
| Debug | 通过 | 74/74 | 无编译警告 |
| Debug + ASan/UBSan | 通过 | 74/74 | 无 sanitizer 报告 |
| Release | 通过 | 74/74 | 完整测试通过 |
| Debug + clang-tidy | 通过 | 74/74 | 最终全量构建零告警 |

Release 独立标签门：

- `unit`：63/63，其中新增 scheduler 用例 12 个；
- `integration`：9/9；
- `pty`：2/2 聚合门。

四个新增 C++ 文件均通过 `clang-format-18 --dry-run --Werror`。

依据仓库根 `AGENTS.md` 权限红线，本任务未执行 `git status`、用于检查工作区的 `git diff`、
暂存、提交或推送。文件正确性通过定向读取、编译、测试、静态检查、格式检查和 SHA256 固化
验证。

## 9. 交付文件

| 文件 | 用途 | SHA256 |
|---|---|---|
| `include/industrial_iot_gateway/scheduler/scheduled_request.hpp` | 请求、时序和统计值对象 | `63a008669d3a781a0bec105a6b051a55da6eab980e170f87a886f1bfdbf78bc1` |
| `include/industrial_iot_gateway/scheduler/poll_scheduler.hpp` | 调度器公共 API 和错误模型 | `f1da9fde3506b318d9042c1bb35170e71a1c4e610d814c15fec24f1891d3c8b6` |
| `src/scheduler/poll_scheduler.cpp` | 计划校验、周期推进、轮转和统计实现 | `f2761604ac0d7ad31cb57a2dfabac8c8bbb5d4c72865517f4ca99a7623c5fd62` |
| `tests/unit/poll_scheduler_test.cpp` | 三从站确定性调度和边界测试 | `90be48ef54ddbffcfbdf516875d689089e31277d1323be43a57115c8fa2c682a` |

另修改根 `CMakeLists.txt` 和 `tests/CMakeLists.txt`，把 scheduler 加入 `gateway_core` 并注册
`gateway_poll_scheduler_test` 单元测试目标。

## 10. 后续边界

T02 向 T03 移交确定性的单在途调度器、请求时序字段和从站统计。以下能力仍未实现，不能由
本记录推定完成：

- `P3-S3-T03` response timeout、有限重试、退避、deadline、offline/recovery 和 freshness；
- `P3-S3-T04` 跨线程有界队列、显式请求公平性、关闭和结构化日志；
- `P3-S3-T05` scheduler、transport、codec/parser 与三个 PTY 的网关主循环集成；
- MQTT、真实 UART/USB-RS485 和 RS485 电气验证。
