# P3-S7-G6-T04：三从站硬件长稳验收

> 文档状态：`APPROVED / PASS`
>
> 工程验收状态：`PASS`
>
> 对应检查点：`CP-F=PASS`、`CP-G=PASS`
>
> 正式候选提交：`c5ddc365ae46c920a78d71ef67ee13fe9de39651`
>
> 编制日期：2026-08-23

## 1. 验收结论

候选提交 `c5ddc365ae46c920a78d71ef67ee13fe9de39651` 已在 Raspberry Pi 4B、真实
USB-RS485、两只 TAS-WS-R00020 和 STM32 NUCLEO-F446RE + Waveshare Shield 组成的三从站
总线上依次完成 3600 秒正式预跑和 28800 秒正式硬件长稳。

两轮运行均由自动 oracle 判定为 PASS。八小时正式运行满足时长、三从站成功率、公平性、MQTT、
资源趋势、温度、throttled、证据容量、关闭时间、证据完整性和运行后恢复门限。

```text
PASS_P3_S7_G6_T04_HARDWARE_LONG_SOAK
CP-F=PASS
CP-G=PASS
G6=IN_PROGRESS
```

本结论只关闭 P3-S7-G6-T04。整个 G6 仍需由 P3-S7-G6-T05 完成证据矩阵、claim ledger、CSV
追踪表和教程索引固化后才能关闭。既有 v0.1.0 本地 Release 是不可变发布事实，仍保持：

```text
PUBLISHED=false
HARDWARE_VALIDATED=false
TAG=null
```

## 2. 验收对象与冻结合同

### 2.1 真实硬件拓扑

```text
Raspberry Pi 4B
  ├── USB-RS485〔首端 120 Ω〕
  │     ── TAS-A〔地址 1〕
  │     ── TAS-B〔地址 2〕
  │     ── Waveshare Shield + STM32〔地址 4，末端 120 Ω〕
  ├── industrial_iot_gateway.service
  └── mosquitto.service（127.0.0.1:1883）
```

### 2.2 运行合同

```text
串口：19200 8E1
稳定路径：/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
TAS-A：地址 1，FC03，2 个任务，4000 ms 周期
TAS-B：地址 2，FC03，2 个任务，4000 ms 周期
STM32：地址 4，FC04，17 个任务，6000 ms 周期
任务总数：21
理论供给负载：约 3.833 次/秒
响应超时：500 ms
总截止时间：5 s
MQTT：3.1.1、QoS 1、本地匿名 Mosquitto
```

长稳过程中没有人工拔线、RESET、修改地址、修改 profile、放宽门限或启动第二个 runner。CAN 不属于
本工作块验收范围，也不存在 CAN 下游消费者。

## 3. 正式运行身份与证据位置

### 3.1 一小时正式预跑

```text
run_id=20260823T032544Z_g6_t04_preflight_c5ddc36_001
profile_kind=preflight
duration=3602.241 s
status=PASS
runner_exit=0
restore_exit=0
failures=[]
```

### 3.2 八小时正式硬件长稳

```text
run_id=20260823T043408Z_g6_t04_release_c5ddc36_001
profile_kind=release
duration=28802.252 s
normal_window=28202.252 s
status=PASS
hardware_long_soak_pass=true
runner_exit=0
restore_exit=0
failures=[]
```

树莓派正式证据根目录：

```text
/home/iot-rp/industrial_iot_gateway_runs/g6/t04/
└── c5ddc365ae46c920a78d71ef67ee13fe9de39651/
```

正式证据包含逐事件 gateway journal、全部 MQTT 捕获、5 秒资源样本、心跳、系统快照、派生 CSV、
summary、failures、manifest、命令记录和 SHA256SUMS。原始层是事实来源，派生数据可以从原始层
重新计算。

## 4. CP-F：3600 秒正式预跑结果

预跑持续 3602.241 秒，所有 enforced oracle 均通过，`failures.json=[]`，SHA256SUMS 全部有效。

| 从站 | 成功率 | 最大成功间隔 | 判定 |
|---|---:|---:|---|
| TAS-A，地址 1 | 1.000 | 4.880 s | PASS |
| TAS-B，地址 2 | 1.000 | 4.882 s | PASS |
| STM32，地址 4 | 1.000 | 2.798 s | PASS |

runner 和恢复步骤均以 0 退出；证据所有权恢复为 `iot-rp:iot-rp`；网关与 Mosquitto 恢复为
active，`NRestarts=0`；没有残留 runner、collector 或 MQTT 订阅器。因此 CP-F 关闭为 PASS，
允许进入八小时正式运行。

## 5. CP-G：八小时正式硬件长稳结果

### 5.1 三从站请求与公平性

| 从站 | 尝试数 | 正常窗口逻辑请求 | 成功逻辑请求 | 成功率 | 最大成功间隔 |
|---|---:|---:|---:|---:|---:|
| TAS-A，地址 1 | 14102 | 14100 | 14100 | 1.000 | 4.886 s |
| TAS-B，地址 2 | 14100 | 14100 | 14100 | 1.000 | 4.895 s |
| STM32，地址 4 | 79901 | 79901 | 79901 | 1.000 | 2.997 s |

```text
逻辑请求吞吐：3.833062693 次/秒
任务覆盖：21/21
每个从站正常窗口成功率门限：>= 0.999
非目标从站最大成功间隔门限：<= 10 s
```

三个从站均满足成功率与公平性门限，没有出现地址冲突、从站饥饿、串口重开或网关意外退出。

### 5.2 MQTT、队列和关闭

正常窗口 MQTT 摘要：

```text
message_count=108118
fresh_count=108100
duplicate_sequences=0
sequence_regressions=0
run_id_mismatch=0
topic coverage=2/2/17
publish_failures=0
dropped=0
drain_expired=0
```

运行时队列和生命周期结果：

```text
scheduler_feedback_delivery_failures=0
scheduler_transition_errors=0
serial_open_successes=1
serial_reopens_after_warmup=0
scheduler_feedback_queue_high_water=1/256
mqtt_queue_high_water=3/1024
unexpected_exits=0
stopped=true
shutdown=194 ms
```

上述结果说明 MQTT 上行没有反向阻塞串口轮询，有界队列没有溢出，SIGTERM 关闭远低于 5 秒门限。

### 5.3 资源、温度和容量

| 指标 | 实际结果 | 结论 |
|---|---:|---|
| RSS 峰值 | 6.765625 MiB | PASS |
| RSS 斜率 | 0.00000508467 MiB/h | 无持续增长趋势 |
| RSS 稳态差值 | 0.02734375 MiB | PASS |
| CPU 平均值 | 1.9071% | PASS |
| CPU P95 | 2.20045% | PASS |
| CPU > 90% 时间 | 0 s | PASS |
| fd 最大值/漂移 | 5 / 0 | PASS |
| 线程最大值/漂移 | 7 / 0 | PASS |
| SoC 最高温度 | 62.8℃ | 低于 80℃门限 |
| throttled 当前位 | 全部 0 | PASS |
| throttled 新增历史位 | 全部 0 | PASS |
| 最小磁盘余量 | 113,818,877,952 bytes | PASS |
| oracle 计入证据大小 | 859,646,076 bytes | 低于 10 GiB |
| 收尾后目录实际大小 | 860,494,667 bytes | 低于 10 GiB |

RSS、fd 和线程没有呈现泄漏趋势；树莓派没有出现当前欠压、当前限频或相对基线新增的历史
throttled 位。证据容量远低于冻结上限，同时保留了足够完整的逐事件数据供复盘与可视化。

## 6. 已解释异常复盘

### 6.1 TAS-A 两次首次尝试超时

八小时内 TAS-A 出现两次尝试级 `response_timeout`：

| 请求 | 第一次尝试 | 隔离处理 | 第二次尝试 | 最终逻辑结果 |
|---|---|---|---|---|
| 41608 | 约 701 ms 超时 | quarantine complete | 约 65 ms 成功 | success |
| 109939 | 约 701 ms 超时 | quarantine complete | 约 72 ms 成功 | success |

这两次事件没有被删除或改写。它们是“尝试级异常”，不是“逻辑请求终态失败”：超时后旧响应上下文
先完成隔离，再执行第二次尝试，最终均成功。地址 1 的正常窗口逻辑成功率仍为 1.000，最大成功间隔
4.886 秒，没有影响地址 2 和地址 4 的公平性。

该结果证明超时、隔离和重试路径在真实总线上被实际触发，但不能据此声明线路永远不会出现超时。
后续复盘应继续保留 `attempt`、请求 ID、单调时间和隔离事件，不能只展示最终成功率。

### 6.2 少量 stale MQTT 消息

包含 warmup、状态消息和收尾窗口的全部 MQTT 原始捕获共 110424 条，其中：

```text
fresh=110400
stale=16
quality=null=8
```

16 条 stale 分布在 STM32 的 `bme280_temperature`（9 条）和 `rs485_error_count`（7 条），两项最终
状态均恢复为 fresh。正常窗口最终摘要中没有重复 sequence、倒退、run_id 混入、发布失败或丢弃，
三个从站的最大成功间隔也没有越界。因此这些消息被判定为可追踪的短暂质量转换，不构成冻结 oracle
失败。

这里的结论边界是：T04 证明质量状态能够被如实发布并恢复，不证明所有时间点的每个字段都恒为
fresh，也不把 stale 当作传感器计量精度问题。

## 7. 证据完整性与运行后恢复

- `summary.status=PASS` 且 `hardware_long_soak_pass=true`；
- 所有 enforced oracle 均通过，`failures.json=[]`；
- SHA256SUMS 对全部正式证据文件校验通过；
- `release_runner_001.exitcode=0`、`release_restore_001.exitcode=0`；
- 证据目录及文件所有权恢复为 `iot-rp:iot-rp`；
- 网关与 Mosquitto 均为 active/running，`NRestarts=0`；
- 网关恢复后的 MainPID 为 64444；
- 没有残留 runner、资源 collector 或 `mosquitto_sub`；
- 正式运行未覆盖、删除或混入一小时预跑证据。

## 8. 能力声明与禁止外推

### 8.1 本工作块可以声明

- 当前冻结树莓派、桌面短线 RS485 拓扑、两只 TAS 和 STM32 的组合完成 1 小时预跑及 8 小时
  连续运行；
- 三个真实 Modbus RTU 从站在正常窗口内逻辑请求成功率均为 100%；
- MQTT 上行、队列、进程资源、温度、关闭和证据完整性满足冻结门限；
- 两次 TAS-A 尝试级超时均由隔离和重试闭环恢复，没有形成逻辑请求失败。

### 8.2 本工作块不能声明

- 不能声明传感器达到计量级精度；本次只验证持续更新、协议解释和趋势合理性；
- 不能外推到长距离、强电磁干扰、不同终端/偏置或其他 RS485 布线；
- 不能声明 CAN 主动通信已经由项目三验证；
- 不能把一次八小时结果写成无限期可靠性证明；
- 不能回写既有 v0.1.0 Release 的 `HARDWARE_VALIDATED=false`；
- 在 T05 关闭前不能声明整个 G6 已完成。

## 9. 后续工作

下一工作块为 P3-S7-G6-T05，按以下顺序执行：

1. 审核本验收候选稿和配套教程；
2. 形成 G6 evidence matrix，串联 T00～T04 的正式证据；
3. 更新 claim ledger 和 CSV 追踪表，新增硬件补充验证事实，不覆盖历史 claim；
4. 更新 `docs/learning/README.md` 学习路线；
5. 明确“发布后硬件补充验证”与不可变 v0.1.0 Release 的边界；
6. 审核通过后再同步公共安全版本到独立仓库。
