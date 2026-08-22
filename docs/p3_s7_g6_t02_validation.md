# P3-S7-G6-T02：三从站集成验收报告

> 状态：`PASS / CP-D_CLOSED / G6_IN_PROGRESS`  
> 执行主机：Raspberry Pi 4B，`iot-rp@192.168.110.36`  
> 串口：`/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`  
> 串口参数：19200 8E1  
> 执行日期：2026-08-23

## 1. 验收边界

T02 的正式目标是地址1的 TAS-A、地址2的 TAS-B 和地址4的 STM32 共用一条 RS485 总线，并由
项目三网关分别输出 JSONL 和 MQTT。未接 Shield/STM32 的双 TAS 组合只是诊断阶梯，其失败不阻塞
最终拓扑；本节记录实际诊断结果，不提前声明三从站通过。

## 2. 双 TAS 基础读取

临时拓扑：

```text
USB-RS485〔首端120Ω〕── TAS-A地址1 ── TAS-B地址2
Shield/STM32未接入
```

| 设备 | 温湿度 | 串口配置 | 地址 | CRC |
|---|---|---|---:|---|
| TAS-A | 55.5%RH、28.1℃ | 19200 8E1 | 1 | 正确 |
| TAS-B | 59.4%RH、27.4℃ | 19200 8E1 | 2 | 正确 |

两只设备均能被独立寻址，没有观察到地址冲突。

## 3. 两分钟交替轮询

执行窗口：2026-08-22T16:06:30Z 至 2026-08-22T16:08:30Z。主站每500ms发出一次请求，地址1和
地址2交替，因此每只从站约每秒被轮询一次。

| 指标 | TAS-A | TAS-B |
|---|---:|---:|
| 请求 | 120 | 120 |
| 成功 | 120 | 120 |
| 成功率 | 1.000000 | 1.000000 |
| 超时 | 0 | 0 |
| CRC错误 | 0 | 0 |
| 畸形帧 | 0 | 0 |
| 最大成功间隔 | 1.050337秒 | 1.050288秒 |

诊断标记：

```text
PASS_G6_T02_DUAL_TAS_DIAGNOSTIC_WITHOUT_SHIELD
```

该标记只说明当前同桌短线临时拓扑能够通信，不替代最终末端、STM32、JSONL或MQTT验收。

## 4. 后续待验收

1. 在总线末端接入启用120Ω终端的 Waveshare Shield 和 STM32；
2. 核对 STM32 地址4、19200 8E1和当前默认固件身份；
3. 三地址原始 Modbus 轮询；
4. 项目三硬件 profile 的完整寄存器覆盖；
5. JSONL 三个 `device_name`；
6. 本地 Mosquitto 上行；
7. 单从站离线时其他从站继续轮询；
8. 成功率、公平性和有界关闭门。

当前不得记录 T02 或 CP-D PASS。

## 5. 最终三从站原始帧检查

最终物理拓扑接入后，三个地址均在19200 8E1下正常响应：

| 对象 | 请求范围 | 结果 |
|---|---|---|
| TAS-A地址1 | 功能码03，`0x0000～0x0001` | 54.1%RH、27.4℃，CRC正确 |
| TAS-B地址2 | 功能码03，`0x0000～0x0001` | 57.4%RH、27.8℃，CRC正确 |
| STM32地址4 | 功能码04，`0x0000` | 签名`0x5035`，CRC正确 |
| STM32地址4 | 功能码04，`0x0008～0x0009` | 采集代数正常读取 |
| STM32地址4 | 功能码04，`0x000C` | 四源存在掩码`0x000F` |
| STM32地址4 | 功能码04，`0x0012～0x0019` | BME280/VEML7700窗口正常读取 |

### 5.1 两分钟三地址轮询

执行窗口：2026-08-22T16:15:36Z 至 2026-08-22T16:17:36Z。地址1、2、4循环调度，每个地址
约每秒一次请求。

| 指标 | TAS-A | TAS-B | STM32 |
|---|---:|---:|---:|
| 请求 | 120 | 120 | 120 |
| 成功 | 120 | 120 | 120 |
| 成功率 | 1.000000 | 1.000000 | 1.000000 |
| 超时 | 0 | 0 | 0 |
| CRC错误 | 0 | 0 | 0 |
| 异常/畸形 | 0 | 0 | 0 |
| 最大成功间隔 | 1.090607秒 | 1.090577秒 | 1.000238秒 |

当前可记录：

```text
PASS_G6_T02_RAW_THREE_SLAVE_POLLING
```

该结果证明物理总线和三个地址的原始只读通信成立。项目三 `gateway_app`、21个轮询任务、JSONL、
MQTT和有界关闭尚未验证，不能据此关闭 T02。

### 5.2 最终拓扑重新上电在线复核

2026-08-23 在用户确认最终三从站拓扑已接入并全部上电后，以大于200ms的请求间隔重新执行15次
只读请求：

| 对象 | 成功/请求 | CRC或帧错误 | 最后读值 |
|---|---:|---:|---|
| TAS-A地址1 | 5/5 | 0 | 55.7%RH、26.0℃ |
| TAS-B地址2 | 5/5 | 0 | 58.2%RH、26.4℃ |
| STM32地址4 | 5/5 | 0 | 签名`0x5035` |

该复核确认当前上电状态与地址身份正确，但仍只属于原始帧在线检查。

## 6. 首轮 gateway_app 探索性运行与阻塞项

首轮运行使用旧 ARM64 `gateway_app` 和尚未冻结的三从站 profile，持续约120秒。进程正常退出，
关闭耗时147ms，stderr为空，但原始 `request_completed` 事件显示：

| 对象 | 成功 | 超时 |
|---|---:|---:|
| TAS-A地址1 | 60 | 67 |
| TAS-B地址2 | 75 | 63 |
| STM32地址4 | 1142 | 0 |

同时出现130次晚响应隔离事件。因此本次运行判定为探索性失败，不能记录 JSONL 集成通过。

### 6.1 根因验证

TAS 手册要求相邻请求间隔不少于200ms，而旧 profile 的21个独立任务产生约21.7次/秒的平均请求
负载。总线成功帧在突发窗口内约每85ms出现一次，超过两只 TAS 的处理能力。

只读对照实验结果如下：

| 实验 | TAS-A | TAS-B | STM32 | 结论 |
|---|---:|---:|---:|---|
| 不限速，30秒 | 1/19 | 0/19 | 186/186 | TAS稳定超时 |
| 全局请求起始间隔200ms，30秒 | 13/13 | 13/13 | 125/125 | 三地址全部成功 |

单寄存器和双寄存器读取的响应时间均约82～133ms且CRC正确，排除了寄存器数量、地址冲突、
500ms响应超时门限和物理接线作为主要原因。根因闭环为：**总线请求起始频率违反 TAS 的
200ms最小间隔合同**。

## 7. 最小修复候选

以下最小修复已经固化到精确候选提交
`0fda289465bb4e915c6c456346b6c928d97340a0`：

1. 在 `protocol_contract` 中增加 `minimum_request_interval_ms`，默认值为0；
2. 串口线程在每次请求开始前执行可被关闭请求中断的全局间隔等待；
3. PTY和既有软件配置未声明该字段时维持原有行为；
4. 硬件 profile 显式冻结为200ms；
5. TAS四个任务调整为4000ms周期、12000ms新鲜度；
6. STM32十七个任务调整为6000ms周期、18000ms新鲜度；
7. 平均供给负载降为约3.83次/秒，低于5次/秒的总线起始容量，同时每个从站的计划成功间隔
   仍低于10秒门限。

新增测试验证硬件合同、负载上限和显式最小请求起始间隔。当前开发工作区质量门结果：

| 质量门 | 结果 |
|---|---|
| Debug完整CTest | PASS，199/199 |
| Release完整CTest | PASS，199/199 |
| ASan/UBSan完整CTest | PASS，199/199 |
| TSan完整CTest | PASS，199/199 |
| clang-format | PASS |
| Clang 18 + clang-tidy全量构建 | 返回0；只出现既有非本次代码告警 |

## 8. 精确候选同步与 ARM64 质量门

候选提交通过 `git archive` 精确同步到树莓派隔离目录：

```text
候选提交：0fda289465bb4e915c6c456346b6c928d97340a0
源码归档 SHA-256：d56656be3b7049178652c3d3d8d1d5bf1c33b09fc0c5c6d45b1db1add1688e3b
硬件 profile SHA-256：c7144d6d48f577009d80e15f6824964dff05e2b2c0a7b18fe78f9aa895847389
运行根：/home/iot-rp/industrial_iot_gateway_runs/g6/t02/0fda289465bb4e915c6c456346b6c928d97340a0
```

树莓派在 Release、MQTT启用和警告视为错误的配置下使用 `-j4` 完成原生构建，完整 CTest
`199/199` 通过，用时182.61秒。

## 9. 三从站 JSONL 正式复验

正式 JSONL 运行持续120秒，stderr为空并正常处理 SIGTERM：

| 指标 | TAS-A地址1 | TAS-B地址2 | STM32地址4 |
|---|---:|---:|---:|
| 成功/请求 | 60/60 | 60/60 | 340/340 |
| 成功率 | 1.000000 | 1.000000 | 1.000000 |
| 最大成功间隔 | 4.860秒 | 4.813秒 | 2.798秒 |
| 覆盖任务 | FC03地址0、1 | FC03地址0、1 | 全部17个FC04地址 |

总计460次请求全部成功，21个轮询任务全部覆盖；超时、CRC错误、晚响应、队列丢弃、合并和
排空超期均为0。`gateway_summary.stopped=true`。

证据目录：

```text
/home/iot-rp/industrial_iot_gateway_runs/g6/t02/0fda289465bb4e915c6c456346b6c928d97340a0/evidence/jsonl
```

## 10. 三从站 MQTT 正式复验

首轮 MQTT 命令使用了含连字符的客户端标识 `g6-t02-0fda289`，违反项目冻结的标识符合同，
因此在串口轮询启动前被拒绝。该次失败保留在 `evidence/mqtt`，没有产生硬件请求，不能计为
总线或 MQTT 运行失败。

改用合法客户端标识 `g6t020fda289` 后重新执行120秒正式复验，结果如下：

| 指标 | 结果 |
|---|---:|
| 串口请求 | 460成功、0失败 |
| MQTT连接 | 1次成功 |
| MQTT发布 | 463成功、0失败 |
| MQTT遥测 | 460条 |
| 网关健康消息 | 3条：online、stopping、offline |
| 队列高水位 | 2 |
| 合并/丢弃/排空超期 | 0/0/0 |
| gateway/subscriber stderr | 均为空 |

分从站结果：

| 指标 | TAS-A地址1 | TAS-B地址2 | STM32地址4 |
|---|---:|---:|---:|
| 成功/请求 | 60/60 | 60/60 | 340/340 |
| 成功率 | 1.000000 | 1.000000 | 1.000000 |
| 最大成功间隔 | 4.808秒 | 4.815秒 | 2.798秒 |

订阅端确认本次 `run_id=477b7dba-7d47-4828-a6db-77187e7b6618` 的463条消息全部使用
`gateway_id=raspberrypi_g6`，覆盖三个 `device_name` 和21个实际寄存器 topic。19个不同
`register_name` 是因为两只 TAS 共用 `relative_humidity` 和 `ambient_temperature` 两个名称。

启动阶段 `rs485_error_count` 的首个样本为 `invalid`，下一轮起连续转为 `fresh`；其原始值
始终为14，没有请求失败。该现象作为启动质量转换事实保留，不把它误记为通信错误。

证据目录：

```text
/home/iot-rp/industrial_iot_gateway_runs/g6/t02/0fda289465bb4e915c6c456346b6c928d97340a0/evidence/mqtt_retry2
```

## 11. 当前检查点

正常路径的三从站 JSONL、MQTT、寄存器覆盖、公平性、队列和有界关闭已经通过。T02 原计划还
要求验证“一个从站离线时其他两个继续轮询”，该物理故障隔离首次执行失败，详见下一节。

```text
PASS_G6_T02_NORMAL_THREE_SLAVE_JSONL_MQTT
BLOCKED / OFFLINE_ISOLATION_FAILED
CP-D=NOT_YET_CLOSED
```

在根因和最小修复闭环前，不把 T02 或 CP-D 写成最终 PASS。

## 12. TAS-A 整体支路断开隔离失败

由于 TAS-A 的 DC 与 A/B 共用同一可拔端子，本次故障刺激定义为：

```text
TAS-A地址1的 DC+/DC-/A/B 整体支路同时断开
```

该操作没有切断 USB-RS485、TAS-B 或 STM32 的主干连接。在线基线为 TAS-A 40/40、TAS-B
39/39、STM32 221/221，均无失败。触发时间为 `2026-08-23T01:31:47.031010715+08:00`。

触发后的关键事件顺序为：

1. TAS-A 请求出现2次 `response_timeout`；
2. 第3次尝试返回 `serial_io_transient`；
3. USB-RS485 稳定设备路径仍存在，没有观察到 USB 断连；
4. 此后不再产生任何串口请求，但网关进程仍存活并持续发布 MQTT；
5. TAS-A 发布2条 `stale/freshness_expired`，没有完成预期的完整离线恢复路径；
6. 进程 CPU 约31%，stderr为空；收到 SIGTERM 后在5秒内正常退出。

最终请求事件统计：

| 对象 | 成功 | 超时 | serial_io_transient | 截止停止时无成功间隔 |
|---|---:|---:|---:|---:|
| TAS-A地址1 | 104 | 2 | 1 | 147.363秒 |
| TAS-B地址2 | 106 | 0 | 0 | 143.066秒 |
| STM32地址4 | 612 | 0 | 0 | 139.075秒 |

非目标从站的截止间隔远超10秒，故障隔离门明确失败。`gateway_summary` 仍记录
`requests_failed=0`，与3条非成功 `request_completed` 事件不一致，这一指标语义也需要在根因
审查中核对。

本次只冻结可直接观察的事实，尚不把 `serial_io_transient` 后串口线程停止的具体代码原因写成
已确认根因。失败证据及14项 SHA-256 清单位于：

```text
/home/iot-rp/industrial_iot_gateway_runs/g6/t02/0fda289465bb4e915c6c456346b6c928d97340a0/evidence/fault_tas_a_retry2
```

当前结论：

```text
FAIL_G6_T02_TAS_A_BRANCH_DISCONNECT_ISOLATION
ROOT_CAUSE_PENDING
```

### 12.1 恢复接线后的独立基线

用户重新接回 TAS-A 的 DC/A/B 四线端子后，使用同一候选提交重新启动一段独立60秒基线：

| 对象 | 成功/请求 | 失败 | 最大成功间隔 |
|---|---:|---:|---:|
| TAS-A地址1 | 30/30 | 0 | 4.799秒 |
| TAS-B地址2 | 30/30 | 0 | 4.875秒 |
| STM32地址4 | 170/170 | 0 | 2.797秒 |

退出码为0，stderr为空，21个轮询任务全部覆盖，`gateway_summary.stopped=true`，证据校验和
通过。该结果确认物理拓扑已恢复，且失败只在“运行中移除 TAS-A 支路”路径触发；它不能抵消
上一节已经确认的故障隔离失败。

恢复证据：

```text
/home/iot-rp/industrial_iot_gateway_runs/g6/t02/0fda289465bb4e915c6c456346b6c928d97340a0/evidence/recovery_baseline
```

## 13. `serial_io_transient` 调度停滞最小修复

### 13.1 源码审查确认的失效链

对候选提交 `0fda289465bb4e915c6c456346b6c928d97340a0` 的运行时路径进行审查后，确认旧实现同时存在：

1. `AttemptSentFeedback` 和 `AttemptResult` 均通过 `try_push` 投递，但返回值被忽略；
2. `mark_attempt_sent` 与 `record_attempt_result` 的调度状态错误没有被处理；
3. `GatewayRuntime` 的调度循环没有调用已有的 `PollScheduler::on_time_advanced()`；
4. 反馈闭环缺失后，调度器可能保留过期的在途请求和已经落在过去的唤醒时间，形成高CPU空转，
   且不再派发后续串口请求。

这条失效链可以解释第12节观察到的“进程和MQTT仍存活、串口请求停止、CPU约31%”现象。旧
硬件证据没有反馈投递结果或调度转换错误字段，因此不能进一步证明当时的唯一瞬时触发点究竟是
队列投递失败还是状态转换拒绝；本报告保留该证据边界。

### 13.2 已实现的最小修复

当前未提交工作区完成以下改动：

- 调度反馈改为最多等待100ms的有界投递；非关闭阶段投递失败时写入关键结构化事件并触发有界
  致命停止；
- 检查 `mark_attempt_sent` 和 `record_attempt_result` 的每个状态转换结果，禁止静默忽略；
- 在调度截止时间之后增加覆盖晚响应隔离、150ms最大晚帧传输和50ms抖动的宽限期，超期仍未
  闭环时调用 `on_time_advanced()`，推进重试或终态；
- 统一正常反馈和截止时间恢复的质量转换及终态统计路径；
- `gateway_summary` 增加 `scheduler_feedback_delivery_failures`、
  `scheduler_transition_errors`、`scheduler_deadline_recoveries` 和
  `scheduler_feedback_queue_high_water`；
- 新增 PTY 集成测试，连续24次零停机串口断开/重连后均要求三个从站恢复进度，并验证反馈投递
  失败、转换错误和队列满为0。

`requests_failed` 的合同经复核为“完成重试策略后的终态失败数”；单次 `response_timeout` 或
`serial_io_transient` 是尝试级结果，不应与该字段逐条相等。

### 13.3 最终源码软件质量门

本机软件构建使用 `-j8`；固定长稳、真实串口和共享端口合同不缩短。最终源码结果如下：

| 质量门 | 结果 |
|---|---|
| MQTT禁用 Debug完整CTest | PASS，188/188 |
| MQTT启用 Debug完整CTest | PASS，200/200 |
| MQTT启用 Release完整CTest | PASS，200/200 |
| ASan/UBSan完整CTest | PASS，200/200，无 sanitizer 报告 |
| TSan完整CTest | PASS，200/200，无竞态报告 |
| Clang 18 + clang-tidy全量构建 | PASS，返回0；本次修改文件无新增告警 |
| clang-format全量检查 | PASS |
| `git diff --check` | PASS |

软件测试只能确认修复路径和进度不变量，不能把第12节真实硬件失败改写为 PASS。下一检查点是由
用户提交当前四个源码/测试文件，取得新的完整候选提交，然后精确同步到树莓派，重新执行：

1. 三从站正常 JSONL/MQTT 基线；
2. TAS-A整体支路断开不少于60秒及恢复；
3. TAS-B整体支路断开不少于60秒及恢复；
4. 非目标从站最大成功间隔、CPU、调度反馈指标、质量状态、MQTT和有界关闭核验。

当前状态：

```text
SOFTWARE_FIX_QUALITY_GATES=PASS
HARDWARE_REVALIDATION=PENDING_COMMITTED_CANDIDATE
CP-D=NOT_YET_CLOSED
```

## 14. 候选提交 `2c16f083` 的硬件复验与第二层竞态

用户提交第一层修复后，候选提交冻结为：

```text
2c16f08322ba095641e55562f31734799d95c6d9
```

该提交精确同步到树莓派后完成 ARM64 Release 构建和完整 CTest `200/200`。三从站120秒正常
基线再次全部通过：TAS-A 60/60、TAS-B 60/60、STM32 340/340，最大成功间隔分别为
4.872秒、4.815秒和2.797秒；总计460次请求全部成功，MQTT发布463次成功、0次失败，调度
反馈投递失败、转换错误和截止恢复均为0。

第二次整体断开 TAS-A 支路时，第一层修复没有再次出现“反馈静默丢失后永久空转”，但暴露了
更窄的排队竞态：

1. TAS-A 请求501的前两次尝试均为 `response_timeout`；
2. 第3次重试进入串口请求队列时，请求总截止时间已经到达；
3. 调度 watchdog 先以 `deadline_exceeded` 将请求501结案，并立即派发 TAS-B 请求523；
4. 约186ms后，串口线程才开始处理请求501并提交 `AttemptSentFeedback`；
5. 调度器此时的在途请求已是523，因此以 `request_id_mismatch` 拒绝迟到反馈；
6. 第一层修复按设计把状态转换错误升级为有界致命停止，没有伪装成正常运行。

本次运行最终统计为519次成功、3次终态失败、`scheduler_transition_errors=1`、
`scheduler_deadline_recoveries=1`、MQTT发布527次成功和0次失败，并在 SIGTERM 下有界退出。USB
内核日志、设备节点 birth/change 时间和稳定路径共同确认 USB-RS485 没有重新枚举。人工故障标记
因聊天与操作延迟晚于实际首个失败事件，所以该标记之后的事件集合为空；报告只按日志实际顺序
陈述，不用该标记伪造精确触发时刻。

失败证据：

```text
/home/iot-rp/industrial_iot_gateway_runs/g6/t02/2c16f08322ba095641e55562f31734799d95c6d9/evidence/fault_tas_a_revalidation
```

恢复 TAS-A 后的独立60秒基线为 TAS-A 30/30、TAS-B 30/30、STM32 170/170，三个最大成功间隔
均低于4.9秒，230次请求和233次 MQTT 发布全部成功，确认物理拓扑恢复。

## 15. 发送前过期的第二层最小修复

### 15.1 测试先行

新增两层回归并在旧实现上确认红灯：

- 调度器单元测试证明 queued 重试的 `deadline_exceeded` 结果原先被
  `invalid_request_state` 拒绝，并残留 in-flight；
- PTY 集成测试使用800ms最小请求间隔和350ms总截止时间，稳定制造“重试已排队但发送前过期”。

### 15.2 实现边界

最小修复没有放宽500ms响应超时、5秒总截止时间或10秒公平性门限：

- `PollScheduler` 只允许 queued 请求在 `observed_at >= deadline` 且结果为
  `deadline_exceeded` 时发送前终止；其他 queued 结果仍拒绝；
- 调度线程只对已经进入 `waiting_response` 的请求执行 watchdog 截止恢复，不再结案已经交给串口
  队列、但尚未真正发送的请求；
- queued 状态最多等待50ms反馈，避免用已经过去的截止时间形成忙循环；
- 串口线程在最小请求间隔结束后、发送前再次检查总截止时间；过期请求不写入串口，记录
  `request_expired_before_send` 并向调度器提交终态结果；
- 实际没有发送的过期请求不更新“上一次请求开始时间”。

### 15.3 软件质量门

本机使用 `-j8`，树莓派使用 `-j4`。当前未提交工作树结果为：

| 质量门 | 结果 |
|---|---|
| x86_64 Debug | 202/202 PASS |
| x86_64 Release | 202/202 PASS |
| x86_64 ASan/UBSan | 202/202 PASS，无 sanitizer 诊断 |
| x86_64 TSan + WSL2兼容入口 | 202/202 PASS，无竞态诊断 |
| MQTT禁用 Release | 190/190 PASS |
| clang-tidy | 全目标构建 PASS；本次修改文件无新增诊断 |
| clang-format与`git diff --check` | PASS |
| ARM64 Release探索性预检 | 原生构建 PASS，202/202 PASS |

ARM64 结果来自未提交工作树归档，只证明当前代码可移植，不是正式候选提交证据。下一步必须由用户
提交这五个源码/测试文件并给出完整提交ID，再精确同步并重新执行 TAS-A 断开、自动恢复以及
TAS-B 对称故障。当前结论仍为：

```text
SECOND_SOFTWARE_FIX_QUALITY_GATES=PASS
FORMAL_HARDWARE_REVALIDATION=PENDING_COMMITTED_CANDIDATE
CP-D=NOT_YET_CLOSED
```

## 16. 候选提交 `b35a01e` 的正式硬件复验

用户将第二层修复提交为以下完整候选提交：

```text
b35a01e25c95fb427fbfa165c4e7f3b030f73b8d
```

该提交以规范化源码归档精确同步至树莓派，主机端与树莓派端归档 SHA-256 均为：

```text
0e48150b3efd19a8805f4d341f7d167b0d2856984ba361b78500d70fa3bfd9d0
```

树莓派使用 `-j4` 完成 ARM64 Release 构建，完整 CTest `202/202 PASS`。正式运行均使用稳定串口
路径 `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`、19200 8E1、200ms全局最小请求起始
间隔和本地 Mosquitto；没有改变500ms响应超时、5秒总截止时间或10秒公平性门限。

### 16.1 三从站正常基线

120秒正常基线结果如下：

| 对象 | 成功/正常窗口请求 | 正常窗口失败 | 最大成功间隔 | 寄存器覆盖 |
|---|---:|---:|---:|---:|
| TAS-A地址1 | 60/60 | 0 | 4.807秒 | 2/2 |
| TAS-B地址2 | 60/60 | 0 | 4.872秒 | 2/2 |
| STM32地址4 | 340/340 | 0 | 2.798秒 | 17/17 |

网关汇总为460次成功、0次失败，MQTT 463次发布成功、0次失败；调度反馈投递失败、状态转换
错误和截止恢复均为0，SIGTERM关闭耗时130ms。STM32在关闭边界出现的一条
`shutdown_cancelled` 不属于正常窗口失败。初版分析器错误地把该关闭事件放入成功率分母并返回4；
原始日志、初版失败文件和修正说明均被保留，修正后的自动判定为 PASS，校验和全部通过。

证据目录：

```text
/home/iot-rp/industrial_iot_gateway_runs/g6/t02/b35a01e25c95fb427fbfa165c4e7f3b030f73b8d/evidence/normal_baseline
```

### 16.2 TAS-A 整体支路断开与恢复

在不重启网关的同一进程内，同时断开 TAS-A 的 DC+/DC-/A/B 整体支路并在故障窗口后恢复：

| 指标 | 结果 |
|---|---:|
| TAS-A连续 `response_timeout` | 43次 |
| TAS-A观测成功间隔（含人工断电窗口） | 234.005秒 |
| `request_expired_before_send` | 17次 |
| 故障期 TAS-B 最大成功间隔 | 5.795秒 |
| 故障期 STM32 最大成功间隔 | 3.503秒 |
| 调度反馈失败/状态转换错误 | 0/0 |
| MQTT发布成功/失败 | 1861/0 |
| 有界关闭 | 146ms |

当前 run_id 的 MQTT 证据完整出现 TAS-A `offline → probing → online`，遥测质量覆盖
`stale`、`offline` 和恢复后的非 retained `fresh`。首次恢复成功之后又取得86次 TAS-A 成功响应，
证明恢复不依赖网关重启。STM32曾出现一次尝试级 `truncated_frame`，随后重试成功，没有形成终态
失败，也没有突破非目标从站10秒门限。自动判定 PASS，`failures.json=[]`，校验和全部通过。

证据目录：

```text
/home/iot-rp/industrial_iot_gateway_runs/g6/t02/b35a01e25c95fb427fbfa165c4e7f3b030f73b8d/evidence/fault_tas_a_final
```

### 16.3 TAS-B 对称故障

使用新的网关进程执行对称场景，在不重启该进程的情况下断开并恢复 TAS-B 整体支路：

| 指标 | 结果 |
|---|---:|
| TAS-B连续 `response_timeout` | 37次 |
| TAS-B观测成功间隔（含人工断电窗口） | 202.989秒 |
| `request_expired_before_send` | 15次 |
| 故障期 TAS-A 最大成功间隔 | 4.722秒 |
| 故障期 STM32 最大成功间隔 | 3.804秒 |
| 调度反馈失败/状态转换错误 | 0/0 |
| MQTT发布成功/失败 | 1841/0 |
| 有界关闭 | 202ms |

当前 run_id 的 MQTT 证据同样完整出现 TAS-B `offline → probing → online`，恢复后的最新遥测为
非 retained `fresh`。恢复阶段关闭边界产生一条 `shutdown_cancelled`，不属于正常窗口失败。自动
判定 PASS，`failures.json=[]`，校验和全部通过。

证据目录：

```text
/home/iot-rp/industrial_iot_gateway_runs/g6/t02/b35a01e25c95fb427fbfa165c4e7f3b030f73b8d/evidence/fault_tas_b_final
```

### 16.4 T02 结论

候选提交 `b35a01e25c95fb427fbfa165c4e7f3b030f73b8d` 已在真实三从站拓扑上关闭第14节发现的
发送前过期竞态。正常基线、TAS-A故障和TAS-B对称故障三个正式证据目录均为 PASS；两个物理
故障期间非目标从站最大成功间隔均低于10秒，MQTT状态恢复和5秒有界关闭同时满足合同。

```text
PASS_P3_S7_G6_T02_THREE_SLAVE_INTEGRATION
CP-D=PASS
G6=IN_PROGRESS
```

该结论只关闭三从站集成检查点。T03 的 USB-RS485拔插、Mosquitto停止恢复、STM32 RESET、
systemd/reboot，以及 T04 的一小时预跑和八小时正式硬件长稳仍未执行，不能据此声明整个 G6
已经通过。
