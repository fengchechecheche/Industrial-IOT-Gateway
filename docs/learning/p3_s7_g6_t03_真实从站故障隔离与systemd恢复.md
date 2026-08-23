# P3-S7-G6-T03：真实从站故障隔离与 systemd 恢复

> 文档性质：中文教学文档
>
> 对应实现：候选提交 `e873b5e31a7dde5990860cf641ee59241e5ef910`
>
> 当前事实：T03 正式 17 个场景全部 PASS；T04 一小时预跑和八小时硬件长稳尚未执行
>
> 适用环境：C++17、Linux ARM64、systemd、Modbus RTU 19200 8E1、MQTT 3.1.1

## 1. 本工作块解决什么问题

T02 已证明三只真实 Modbus 从站在正常状态下能够共享一条 RS485 总线，也证明单只 TAS 支路断开
时另外两个从站仍可工作。T03 进一步回答的是：系统中不同层级的组件真的发生故障时，网关会不会
停滞、误报、无限重启，或者必须依靠人工重启才能恢复？

本工作块覆盖六类故障边界：

```text
单个从站故障：TAS-A/TAS-B 断电并断开 A/B、STM32 RESET
串口链路故障：USB-RS485 从树莓派拔出
上行服务故障：Mosquitto 停止
进程生命周期：网关收到 SIGTERM
操作系统生命周期：树莓派受控 reboot 与 systemd 开机自启
```

它在项目中的角色不是增加新业务功能，而是验证“已有功能遇到故障仍能受控运行和恢复”。

## 2. 所需术语和基础概念

- **故障域（fault domain）**：一个故障直接影响的范围。例如 TAS-A 断电应影响地址 1，不应让
  地址 2 和地址 4 永久停止。
- **故障隔离（fault isolation）**：把影响限制在故障域内，非目标设备仍能取得服务。
- **恢复时间（recovery time）**：故障解除后，到目标重新出现有效成功事件所经历的单调时间。
- **测试刺激（stimulus）**：人为制造的受控故障，如拔出 USB-RS485 或停止 broker。
- **测试预言机（test oracle）**：把实际证据转换为 PASS/FAIL 的规则，而不是人的主观印象。
- **正常窗口（normal window）**：排除已标记故障区间后的稳定运行时段。
- **MainPID**：systemd 记录的服务主进程 PID。
- **NRestarts**：systemd 统计的自动重启次数。
- **boot ID**：Linux 每次启动生成的唯一标识，用来区分重启前后的 journal。
- **稳定设备路径（stable device path）**：本项目使用 `/dev/serial/by-id/...`，避免依赖可能变化的
  `/dev/ttyUSB0`。
- **幂等恢复（idempotent recovery）**：恢复动作即使重复检查，也不会让系统进入不同或更坏的状态。

## 3. 故障隔离的工作原理

### 3.1 为什么单个从站超时不应堵住整条总线

RS485 是共享半双工总线，主站一次只能执行一个 Modbus 事务。某个从站不响应时，主站必须等到
响应超时或总截止时间后释放当前事务，再调度其他地址。如果实现把“当前请求”永久留在
`in-flight` 状态，即使进程仍然存活，整条总线也会停止前进。

本项目通过以下边界避免永久停滞：

1. 每次响应有 500 ms 超时；
2. 请求包含 5 秒总截止时间；
3. 串口线程在真正发送前检查请求是否已过期；
4. 调度反馈使用有界队列，投递或状态转换失败不能静默忽略；
5. 目标失败进入重试或终态后，调度器继续选择其他从站。

因此 TAS-A 断开会产生地址 1 超时，但地址 2 和地址 4 仍能轮询。本次正式结果中，TAS 故障时
非目标最大成功间隔均低于 5.8 秒，未突破 10 秒门限。

### 3.2 尝试级错误与终态失败

一次 `response_timeout` 只表示本次尝试没有收到合法响应。可靠性策略可能随后重试；只有所有允许
尝试结束后仍未成功，才形成终态失败。把二者混为一谈会产生两个问题：

- 将正常重试误报成整个设备故障；
- 只看最终成功而忽略线路正在频繁超时。

结构化日志同时保留请求 ID、尝试次数、从站地址、错误类别和最终结果，使 runner 能分别判断
“刺激是否真的发生”和“系统最终是否恢复”。

## 4. systemd 如何管理网关生命周期

当前 unit 的关键合同为：

```ini
Restart=on-failure
RestartPreventExitStatus=2 3 4 5 6 7
TimeoutStopSec=5s
```

`Restart=on-failure` 并不意味着任何情况下都重启。正常 `systemctl stop` 使程序处理 SIGTERM、完成
有界排空并以 0 退出，systemd 应保持服务 inactive。异常退出才属于自动重启候选；配置错误对应的
特定退出码又被 `RestartPreventExitStatus` 排除，防止错误配置造成重启风暴。

T03 的三轮停止结果为 201、200、201 ms，均出现 `gateway_summary` 和 `stopped=true`，停止后
`MainPID=0`。显式启动后，三从站与 MQTT 均恢复。

### 4.1 为什么要同时检查 active、MainPID 和 NRestarts

只看到 `active` 不足以证明稳定：服务可能在反复崩溃和重启之间短暂处于 active。组合检查能够
区分不同情况：

| 观察 | 可能含义 |
|---|---|
| active，MainPID 稳定，NRestarts=0 | 服务持续运行 |
| active，MainPID 变化，NRestarts 增加 | 发生过自动重启 |
| activating 与 failed 反复出现 | 可能存在重启风暴 |
| inactive，退出码 0 | 正常停止，符合 SIGTERM 合同 |

本次所有不应重启的故障场景均保持 `NRestarts=0`。

## 5. 各类故障为什么需要不同的恢复策略

### 5.1 TAS 支路断开

拔下 TAS 的一体式 DC+/DC-/A/B 端子，同时移除供电和信号。网关会观察到目标地址超时，并把
设备质量从 fresh 推进到 stale/offline。端子恢复后，目标先进入 probing，再由有效 Modbus 响应
恢复 online，新的遥测必须是非 retained 的 fresh 值。

这里的关键不是让目标“立即成功”，而是在等待目标恢复期间继续公平服务其他地址。

### 5.2 STM32 RESET

RESET 的时间可能短于一次轮询周期，所以不能只用“是否出现一次超时”判断刺激。runner 同时检查
可观测的重启迹象、17 项寄存器覆盖和恢复后的 fresh 消息。三轮恢复时间约为 6.0～6.8 秒，
TAS-A/TAS-B 的最大成功间隔均低于 5.3 秒。

### 5.3 Mosquitto 停止

MQTT broker 属于上行路径，不拥有 RS485 串口。`GatewayRuntime` 中的串口轮询与
`MqttPublishSink` 的网络活动分离，因此 broker 停止时串口线程应继续采集。

断线期间，应用只能在有界缓存内合并普通遥测并保留关键状态，不能无限积压。恢复连接后重新发布
health/status 和仍有效的数据。本次三轮 broker 故障中，三个从站正常窗口成功率均为 1.000，
MQTT 在 5.565～17.648 秒内恢复，`drain_expired=0`。

### 5.4 USB-RS485 拔出

USB 设备拔出后，已有文件描述符会失效，`/dev/serial/by-id/...` 也会消失。网关不能把旧 fd 当作
永久有效，也不能切换到写死的 `/dev/ttyUSB0`。它需要关闭失效串口、按有界策略重新打开稳定路径，
并在设备重新枚举后恢复事务。

`GatewayRuntime` 的统计字段 `serial_open_successes` 用来证明发生了新的成功打开。三轮测试中
by-id 均先消失后恢复，该计数每轮增加 1，恢复时间为 5.521～8.580 秒，进程没有重启。

### 5.5 树莓派 reboot

reboot 跨越了进程生命周期，因此需要外部控制端观察。重启前先冻结：

```text
候选提交 + runner SHA-256 + 安装文件 SHA-256 + boot ID
```

重启后只有同时满足以下条件，才能证明是同一候选在新 boot 中自动恢复：

- boot ID 改变；
- 安装哈希未改变；
- gateway 和 Mosquitto 都 enabled/active；
- journal 事件全部属于新 boot；
- 网关在 120 秒内 ready；
- 三个从站和 MQTT 都产生新成功事件；
- 600 秒观察内 `NRestarts=0`、无 failed unit、无温度或 throttled 异常。

本次网关在开机后约 22.16 秒 ready，观察 602.52 秒，温度峰值 50.6℃。

## 6. 本项目中的数据流与线程角色

```text
PollScheduler
  ↓ 产生带地址、截止时间和重试策略的请求
Bounded request queue
  ↓
GatewayRuntime 串口执行路径（唯一串口所有者）
  ↓ Modbus RTU 请求/响应、CRC 和帧校验
EventPipeline / FreshnessTracker
  ↓ 工程量、fresh/stale/offline 状态
PublishSink
  ├── JsonlPublishSink
  └── MqttPublishSink（独立网络活动和有界缓存）
```

systemd 位于进程外部，负责启动、停止和失败策略；Mosquitto 位于 MQTT sink 外部；USB-RS485 和
三个从站位于串口边界外部。T03 分别破坏这些边界，验证故障不会越过不应受影响的层。

## 7. 关键类、接口和源码执行过程

### 7.1 产品代码

- `include/industrial_iot_gateway/runtime/gateway_runtime.hpp`：定义 `GatewayRuntime` 和运行统计，
  包括 `serial_open_successes`；
- `src/runtime/gateway_runtime.cpp`：执行串口打开、请求事务、`request_completed` 结构化事件和恢复；
- `include/industrial_iot_gateway/scheduler/poll_scheduler.hpp`：定义 `PollScheduler` 的调度状态；
- `include/industrial_iot_gateway/concurrency/bounded_queue.hpp`：提供有界队列；
- `src/mqtt/mqtt_publish_sink.cpp`：管理 MQTT 连接、发布和 `mqtt_connected` 事件；
- `src/app/main.cpp`：输出 `gateway_ready` 和最终 `gateway_summary`；
- `packaging/systemd/industrial_iot_gateway.service`：冻结重启与 5 秒停止合同。

### 7.2 验收工具

- `tools/hardware/g6_t03_contract.py`：把观察值转换成 oracle 和失败原因；
- `tools/hardware/g6_t03_runner.py`：管理场景状态机、证据目录、最终 summary 和校验和；
- `tools/hardware/g6_t03_reboot_controller.py`：检查 boot ID、ready 时间、三从站、MQTT、温度、
  throttled 和安装哈希；
- `config/hardware/g6_t03_profile.json`：冻结场景、设备和门限。

正式执行流程为：

```text
preflight
→ initialize
→ mark-baseline
→ record-action(trigger)
→ evaluate
→ record-action(restore/complete)
→ prepare-reboot
→ 用户单独授权 reboot
→ post-reboot
→ finalize
```

runner 的返回码是合同的一部分：oracle 失败必须返回非零，避免“日志里已经失败，但自动化仍显示
成功”。

## 8. 为什么选择当前方案

1. **每次只注入一个故障**：便于确定故障域和恢复原因；
2. **使用单调时间**：不受 NTP 或系统时钟调整影响；
3. **使用结构化事件和事件 ID**：摘要可以追溯到原始请求与 MQTT 消息；
4. **使用稳定 by-id 路径**：USB 重新枚举后不依赖易变的 tty 编号；
5. **正常窗口与故障窗口分开**：既不把预期超时误判为系统失败，也不删除原始错误；
6. **reboot 前冻结哈希**：防止重启前后实际运行了不同候选；
7. **正式证据统一 SHA-256**：能够发现后续误改或缺失文件。

## 9. 常见错误、失败模式和安全边界

### 9.1 常见工程错误

- 只看进程存在，不检查请求是否仍在前进；
- 只看最终恢复，不检查非目标从站是否被饿死；
- 把故障窗口的预期超时算入正常成功率，或反过来直接删除错误日志；
- USB 重插后临时改用 `/dev/ttyUSB0`，掩盖稳定路径恢复失败；
- 看到 systemd active 就忽略 `NRestarts` 和 MainPID 变化；
- 用旧 retained MQTT 消息冒充当前运行的恢复消息；
- reboot 前后不核对候选提交和安装哈希；
- 把一次 600 秒重启观察写成八小时长稳。

### 9.2 安全边界

- 一次只操作一个目标；
- TAS 只拔确认过的一体式低压端子，不操作 12V 电源市电侧；
- USB-RS485 只拔 USB 侧，不带电拆 A/B，不短接或反接；
- STM32 只按明确标识的 RESET，不重新烧录或短接引脚；
- reboot 必须取得当次明确授权；
- 不执行 A/B 短路、过压、反接、强制断电或未规划的 CAN 故障。

## 10. 本工作块完成与未完成的能力

已经完成：

- TAS-A/TAS-B 各累计 3 次支路故障恢复；
- STM32 RESET 3/3；
- Mosquitto 停止与恢复 3/3；
- USB-RS485 拔插与恢复 3/3；
- SIGTERM 停止与显式恢复 3/3；
- 真实三从站条件下 systemd reboot、自启和 600 秒稳定观察；
- 17 个正式场景、清理门和 579 项 SHA-256 全部 PASS。

尚未完成：

- 一小时真实硬件预跑；
- 八小时真实硬件正式长稳；
- 工业现场长距离、强干扰和计量精度验证；
- 项目三 CAN 主动通信验证；
- G6 最终证据/claim/CSV 固化和硬件补充版本。

因此当前状态是 `CP-E=PASS`、`G6=IN_PROGRESS`。

## 11. 后续学习资料

以下资料均为协议或平台的一手资料，访问日期为 2026-08-23：

- [Modbus Organization：Modbus Serial Line Protocol and Implementation Guide V1.02](https://www.modbus.org/docs/Modbus_over_serial_line_V1_02.pdf)：学习 RTU 帧、主从事务、串行链路和 RS485 实施建议；
- [Modbus Organization：Specifications and Implementation Guides](https://www.modbus.org/modbus-specifications)：查找 Modbus Application Protocol V1.1b3 等规范；
- [OASIS：MQTT Version 3.1.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)：学习 CONNECT、PUBLISH、QoS、RETAIN、会话和遗嘱消息；
- [systemd.service 官方手册](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)：学习 `Restart=`、`TimeoutStopSec=` 和服务状态；
- [journalctl 官方手册](https://www.freedesktop.org/software/systemd/man/latest/journalctl.html)：学习按 unit、boot 和 cursor 提取日志；
- [Linux man-pages](https://www.kernel.org/doc/man-pages/)：继续学习 `termios(3)`、`poll(2)`、`clock_gettime(2)` 和进程信号。

这些资料解释协议和平台机制；本项目是否通过仍以候选提交、正式 run、原始证据和自动 oracle 为准。

