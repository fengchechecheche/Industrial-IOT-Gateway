# P3-S7-G6-T03：真实从站故障隔离与 systemd 恢复验收报告

> 文档状态：`正式维护版本`
>
> 工程验收状态：`PASS`
>
> 对应检查点：`CP-E=PASS`
>
> 候选提交：`e873b5e31a7dde5990860cf641ee59241e5ef910`
>
> 编制日期：2026-08-23

## 1. 验收结论

候选提交 `e873b5e31a7dde5990860cf641ee59241e5ef910` 已在 Raspberry Pi 4B、真实
USB-RS485、两只 TAS-WS-R00020 和 STM32 NUCLEO-F446RE + Waveshare Shield 组成的三从站
总线上完成 P3-S7-G6-T03 正式验收。

正式 runner 共完成 17 个场景，未出现缺失场景、失败场景或未关闭失败；清理门通过，579 个证据
文件的 SHA-256 全部校验通过。

```text
PASS_P3_S7_G6_T03_HARDWARE_FAULT_ISOLATION_AND_SYSTEMD_RECOVERY
CP-E=PASS
G6=IN_PROGRESS
```

该结论只关闭“真实从站故障隔离与 systemd 恢复”工作块。P3-S7-G6-T04 的一小时预跑和八小时
正式硬件长稳尚未执行，因此不能声明整个 G6 已通过，也不能修改既有 v0.1.0 Release 中的
`HARDWARE_VALIDATED=false`。

## 2. 验收对象与冻结合同

### 2.1 硬件与服务拓扑

```text
Raspberry Pi 4B
  ├── USB-RS485〔首端 120 Ω〕
  │     ── TAS-A〔地址 1〕
  │     ── TAS-B〔地址 2〕
  │     ── Waveshare Shield + STM32〔地址 4，末端 120 Ω〕
  ├── industrial_iot_gateway.service
  └── mosquitto.service（127.0.0.1:1883）
```

运行合同：

```text
串口：19200 8E1
稳定路径：/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
TAS-A：tas_env_01，地址 1，功能码 03
TAS-B：tas_env_02，地址 2，功能码 03
STM32：stm32_condition_node，地址 4，功能码 04
MQTT：3.1.1，QoS 1，本地匿名 Mosquitto
全局最小请求起始间隔：200 ms
响应超时：500 ms
总截止时间：5 s
```

### 2.2 正式运行身份

| 项目 | 结果 |
|---|---|
| 主机 | Raspberry Pi 4B，AArch64 |
| 内核 | Linux 6.8.0-1061-raspi |
| Python | 3.12.3 |
| 候选提交 | `e873b5e31a7dde5990860cf641ee59241e5ef910` |
| 正式 run ID | `20260822T214326Z_g6_t03_e873b5e_001` |
| Runner SHA-256 | `76f8a3ceabf67c275abb7356f3b0683975bd3680b0cd61b39d01ef5b540e0a8c` |
| 证据大小 | 139,500,939 字节，约 133.0 MiB |
| SHA256SUMS 条目 | 579 |

正式证据目录：

```text
/home/iot-rp/industrial_iot_gateway_runs/g6/t03/e873b5e31a7dde5990860cf641ee59241e5ef910/artifacts/hardware/g6-t03/20260822T214326Z_g6_t03_e873b5e_001
```

## 3. 证据与判定方法

验收使用项目仓库中的以下正式工具：

```text
config/hardware/g6_t03_profile.json
tools/hardware/g6_t03_contract.py
tools/hardware/g6_t03_runner.py
tools/hardware/g6_t03_reboot_controller.py
```

每个场景由结构化 journal、MQTT 捕获、单调时间、systemd 属性和人工动作确认共同限定。人工确认
只表示物理动作已经发生；超时、恢复和公平性指标由网关事件自动计算，未通过的 oracle 会使 runner
返回非零，不能仅凭人工阅读日志写成 PASS。

正常窗口继续采用以下门限：

```text
在线从站请求成功率 >= 0.999
在线非目标从站最大成功间隔 <= 10 s
目标/串口/MQTT 恢复 <= 120 s
SIGTERM 关闭 <= 5 s
systemd ready <= 120 s
reboot 后连续观察 >= 600 s
NRestarts = 0
未关闭失败 = 0
```

目标设备在故障注入窗口内产生的预期超时不计入正常窗口成功率，但原始事件仍保留在证据中。

## 4. 场景验收结果

### 4.1 TAS-A、TAS-B 支路断开与恢复

T02 已分别完成每只 TAS 的第 1 个正式周期；T03 补做周期 2 和周期 3，使每只 TAS 累计达到
3 个正式周期。

| 场景 | 目标超时 | 非目标最大成功间隔 | 恢复时间 | MQTT 状态序列 | 结果 |
|---|---:|---|---:|---|---|
| TAS-A 周期 2 | 26 | TAS-B 5.199 s；STM32 2.997 s | 0.316 s | offline → probing → online | PASS |
| TAS-A 周期 3 | 29 | TAS-B 5.292 s；STM32 4.005 s | 1.641 s | offline → probing → online | PASS |
| TAS-B 周期 2 | 23 | TAS-A 5.782 s；STM32 3.303 s | 0.713 s | offline → probing → online | PASS |
| TAS-B 周期 3 | 17 | TAS-A 5.210 s；STM32 2.798 s | 1.847 s | offline → probing → online | PASS |

四个 T03 场景均满足：

- 网关 MainPID 保持不变，`NRestarts=0`；
- 调度反馈投递失败和状态转换错误均为 0；
- 恢复后的 fresh 遥测 `value_is_retained=false`；
- 非目标从站最大成功间隔均未超过 10 秒。

结合 T02 周期 1，可判定 TAS-A、TAS-B 各累计 3/3 PASS。

### 4.2 STM32 RESET 3 轮

| 轮次 | RESET 刺激 | TAS-A 最大间隔 | TAS-B 最大间隔 | STM32 恢复 | 寄存器覆盖 | 结果 |
|---|---|---:|---:|---:|---:|---|
| 1 | 已观测 | 4.806 s | 4.813 s | 6.706 s | 17/17 | PASS |
| 2 | 已观测 | 5.218 s | 5.225 s | 6.000 s | 17/17 | PASS |
| 3 | 已观测 | 4.800 s | 4.817 s | 6.801 s | 17/17 | PASS |

三轮均出现可判定的复位刺激；STM32 恢复后重新产生 fresh MQTT 遥测，网关进程没有重启。

### 4.3 Mosquitto 停止与恢复 3 轮

| 轮次 | 三从站正常窗口成功率 | 最大成功间隔（地址 1/2/4） | MQTT 恢复 | 结果 |
|---|---|---|---:|---|
| 1 | 1.000 / 1.000 / 1.000 | 4.805 / 4.814 / 2.799 s | 8.667 s | PASS |
| 2 | 1.000 / 1.000 / 1.000 | 4.873 / 4.807 / 2.799 s | 5.565 s | PASS |
| 3 | 1.000 / 1.000 / 1.000 | 4.876 / 4.871 / 2.799 s | 17.648 s | PASS |

broker 停止期间串口轮询持续；MQTT 队列保持有界，恢复后重新连接并恢复 fresh 遥测，
`drain_expired=0`。网关 MainPID 保持不变。

### 4.4 USB-RS485 拔出与恢复 3 轮

| 轮次 | by-id 消失/恢复 | `serial_open_successes` 增量 | 恢复后成功请求（地址 1/2/4） | 恢复时间 | 结果 |
|---|---|---:|---|---:|---|
| 1 | 是/是 | 1 | 122 / 122 / 698 | 8.580 s | PASS |
| 2 | 是/是 | 1 | 92 / 92 / 517 | 5.705 s | PASS |
| 3 | 是/是 | 1 | 98 / 96 / 529 | 5.521 s | PASS |

三轮中稳定 by-id 路径均按预期消失并恢复到字符设备，网关无需人工重启即可重新打开串口并恢复
全部三个从站；MainPID 保持不变，`NRestarts=0`。

### 4.5 SIGTERM 停止与显式恢复 3 轮

| 轮次 | `systemctl stop` 结果 | 停止耗时 | 停止态 | 显式启动后恢复 | 残留进程 | 结果 |
|---|---:|---:|---|---|---:|---|
| 1 | 0 | 201 ms | inactive，MainPID=0 | 是 | 0 | PASS |
| 2 | 0 | 200 ms | inactive，MainPID=0 | 是 | 0 | PASS |
| 3 | 0 | 201 ms | inactive，MainPID=0 | 是 | 0 | PASS |

三轮均生成 `stopped=true` 的 `gateway_summary`，正常停止后未被 `Restart=on-failure` 重新拉起；
显式 `start` 后三从站和 MQTT 均恢复。

### 4.6 systemd 开机自启与受控 reboot

重启前冻结候选提交、runner 和安装文件 SHA-256，并在获得用户当次明确授权后仅执行一次
`sudo reboot`。

| 项目 | 实际结果 | 门限 | 判定 |
|---|---:|---:|---|
| 重启前 boot ID | `ed24a4c1-a727-4210-bc56-59e753c6a1d5` | — | 记录完成 |
| 重启后 boot ID | `28a1836a-3856-407f-a7d9-6ed0602959e2` | 必须变化 | PASS |
| 网关 ready | 22.160 s | <= 120 s | PASS |
| 连续观察 | 602.520 s | >= 600 s | PASS |
| 地址 1/2/4 成功请求 | 382 / 382 / 2159 | 各 >= 1 | PASS |
| 当前运行 MQTT 消息 | 2312 | >= 1 | PASS |
| `NRestarts` | 0 | 0 | PASS |
| failed units | 0 | 0 | PASS |
| SoC 温度峰值 | 50.6℃ | < 80℃ | PASS |
| throttled 当前位 | 0 | 0 | PASS |
| throttled 新增历史位 | 0 | 0 | PASS |

重启后 `industrial_iot_gateway.service` 和 `mosquitto.service` 均为 enabled/active，稳定串口路径可由
服务用户读写；journal 只来自新的 boot，未检测到重启风暴。

## 5. 总体结果与清理门

正式总报告：

```text
status=PASS
scenario_count=17
failed_scenarios=[]
missing_scenarios=[]
unclosed_failures=0
cleanup_ok=true
G6_T03_FINALIZE_EXIT=0
```

清理结果：

- 临时构建目录为 0；
- 树莓派和 Windows 侧 T03 临时 helper 为 0；
- `.pyc`、`.pyo` 和 `__pycache__` 为 0；
- 残留测试进程为 0；
- 三从站拓扑已恢复，网关和 Mosquitto 保持 active；
- 根分区剩余 115,119,755,264 字节，高于 15 GiB 门限；
- 579 个 SHA-256 条目全部通过。

`working/` 中被 `SHA256SUMS` 覆盖的场景原始材料属于正式证据，不作为临时目录删除。

## 6. 能力声明边界

本工作块可以声明：

- 项目三网关在当前桌面短线三从站台架上，通过了 TAS 单支路断开、STM32 RESET、Mosquitto
  停止、USB-RS485 拔插、SIGTERM 和受控 reboot 的故障隔离与自动恢复验收；
- systemd 能在真实硬件启动条件下完成开机自启，且 600 秒观察内无重启风暴；
- 故障期间的非目标从站公平性、恢复后的 MQTT fresh 状态和 5 秒有界关闭满足冻结合同。

本工作块不能声明：

- 已完成一小时预跑或八小时真实硬件长稳；
- 已验证长距离、强干扰或工业现场 RS485 布线；
- TAS 测量值达到计量精度；
- 项目三已验证 CAN 主动通信；
- G6、硬件补充版本或 GitHub Release 已完成。

## 7. 后续工作

下一工作块为：

```text
P3-S7-G6-T04：一小时预跑与八小时正式硬件长稳
```

只有 T04 正式长稳 PASS 后，才进入 P3-S7-G6-T05 的 evidence matrix、claim ledger、CSV 和后续
硬件验证版本边界固化。
