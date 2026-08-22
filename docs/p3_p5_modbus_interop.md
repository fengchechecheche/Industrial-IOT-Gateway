# 项目三与项目五 Modbus/RS485 联调报告

> 任务：`P5-P3-INT-01`  
> 结果：`PASS_BOUNDED_INTEROP`  
> 发布边界：`PUBLISHED=false / HARDWARE_VALIDATED=false / TAG=null`

## 1. 验收范围

2026-08-21 在 Ubuntu-24.04-Gateway x86_64 中，使用项目三网关作为唯一 Modbus RTU
主站，通过新 CH340 USB-RS485 读取项目五 NUCLEO-F446RE 节点。节点地址固定为 4，串口为
19200 bit/s、8E1，接线为短线、共地、`A→A`、`B→B`。本轮只使用 `0x04` 输入寄存器读取，
没有提供任何 `--write-*` 参数，没有执行地址迁移或 MQTT 下行控制。

身份绑定：

- 项目三 profile：`[039] 17d67873f83488b08ea0eee0fa28c8722b0913d6`；
- 项目五源码：`[067] a173717deb0814019a51a73f59834b9c3c5fd309`；
- 项目五默认 Debug ELF SHA-256：
  `d076ddf743020fe1a043e776ba3196ea1f02153a17c5d98451cc722d6ac0018f`；
- USB-RS485：CH340，VID:PID `1a86:7523`，完整设备序列号未记录；
- WSL 稳定路径：`/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`。

## 2. JSONL 只读联调

省略全部 MQTT 参数，以现有 JSONL sink 连续运行约 141.145 秒。结果为：

- 17 个 profile 地址全部得到有效响应；
- 1946/1946 个 Modbus 请求成功，失败 0；
- 1946 条 telemetry 均为 `valid_sample`，17 个字段均进入 `fresh`；
- 串口只打开一次，没有重开、离线或超时；
- 运行中人工按一次 NUCLEO RESET，网关持续恢复读取，最大相邻完成间隔为 856 ms；
- 最终收到 `stopped=true` 的有界停止摘要。

该结果证明指定提交、profile、转换器和短线接线上的真实地址 4 读取与 JSONL 投影成立，
不声明严格复位恢复上界、长期零错误率或其他从站兼容性。

## 3. 本地 MQTT 投影

JSONL 通过后，使用同一串口与 profile，启用项目三 MQTT Debug 构建并连接本地匿名
Mosquitto `127.0.0.1:1883`，运行约 60 秒。订阅范围限定为
`industrial_iot_gateway/devices/stm32_condition_node/#`。

结果：

- 822/822 个 Modbus 请求成功，失败 0；
- 网关记录 MQTT connected 1 次、publish success 825、publish failure 0；
- 订阅器收到 822 条 telemetry，覆盖全部 17 个 topic；
- 822 条消息全部为 `quality=fresh`、`quality_reason=valid_sample`、`slave_id=4`；
- BME280、VEML7700、ADXL345 和 health 必需主题各收到 60 条；
- 代表值首尾分别为：温度 `23.36→23.35 degC`、照度 `46.234→44.352 lux`、
  ADXL345 X 轴 `-0.051→-0.051 g`、health `1→1`；
- 设备签名为 `20533`，四源 present mask 为 `15`；
- queue high-water 为 2，coalesced、dropped、drain-expired 均为 0，最终有界停止。

因此可以声明“真实项目五 STM32 数据已由指定项目三网关投影至本地 MQTT”。该结论不覆盖
远程 broker、TLS、凭据、MQTT 下行、断网恢复或生产部署。

## 4. 双总线并发补充复验

在上述 Modbus/RS485 与 MQTT 验收之后，又将新 CH340 USB-RS485 和
candleLight USB-CAN 同时挂载到 `Ubuntu-24.04-Gateway`。项目三继续使用同一地址 4
只读 profile；CAN 由 Gateway 环境中的 Python 标准库 SocketCAN 观察器读取，不修改项目三
业务程序。`can0` 明确配置为 500 kbit/s、Host sample point `0.75`、公共 GND 和 USB-CAN
`120R`，准入时为 ERROR-ACTIVE 且统计全零。

125 秒正式并发轮次和随后 25 秒聚焦确认完成 2060/2060 次 Modbus 请求，失败 0，覆盖全部
17 个输入寄存器地址；六种 CAN 周期 ID 同时持续出现。该轮使用的临时 Host 发送片段在收到
`0x541` 后立即关闭 raw CAN socket，曾出现一次 error-warning，因此不作为最终 CAN 诊断工具。

随后进行的十分钟复验完成 8291/8291 次 Modbus 请求，失败 0。前五分钟只被动接收 CAN，
warning、passive 和 bus-off 均保持为 0；在使用同类临时短生命周期 sender 发送一个有效
`0x540` 后，虽然收到了唯一正确 `0x541`，Host 状态计数仍升至 8 warning、33 passive 和
210 bus-off，停止主动发送后不再增长，Modbus 和 CAN 周期 RX 继续推进。

上述早期 A/B 现象一度支持“短生命周期 sender 与关闭时序相关”的候选假设，但后续 T1～T4
十分钟矩阵推翻了把 socket 关闭视为必要条件的结论：单一长驻 socket、监控 socket 加第二个
全程长驻 socket、逐次创建并立即关闭 sender，以及发送后延迟 5 秒关闭 sender，四种模式均可
出现 CAN 错误帧或状态计数增长。因而当前证据只支持异常与本台架的 Host 主动 CAN 路径相关，
不能把精确根因唯一归给 socket 生命周期、candleLight、`gs_usb`、USB-IP、项目五固件或某个
单独组件。

T5 随后将产品范围收敛为“RS485 主动双向轮询 + CAN 被动遥测”，持续约 600 秒并通过：项目三
Modbus 请求 8768 次成功、失败 0；六种周期 CAN ID 各收到 610 帧；专用错误帧为 0，warning、
passive、bus-off 计数均无增长，Host 主动 CAN TX 为 0。该结果证明产品范围的双总线并发成立，
不证明 Host→STM32 CAN 应用请求可靠。项目三当前仍只在应用层处理 Modbus；CAN 观察由 Gateway
环境中的独立 SocketCAN 工具完成。T1～T5 的权威明细见项目五
`docs/can_socket_lifecycle_matrix.md`，成功原始 JSONL 在形成摘要后已删除。

## 5. Raspberry Pi 4B/ARM64 实物补验

2026-08-22 在物理 Raspberry Pi 4B（Ubuntu 24.04.4、AArch64）上使用项目三 `[040]
b28191e0e4a179bb9bcdb245a73d272a70a6c73b`、既有 ARM64 发布包和地址 4 只读 profile，连接
同一新 CH340 与 candleLight USB-CAN；项目五为 `[069]
2789d740e37da6cce4c641a7838fc28ad2b80b84`，默认 Debug ELF SHA-256 仍为
`d076ddf743020fe1a043e776ba3196ea1f02153a17c5d98451cc722d6ac0018f`。

三条有界路线均通过：

- JSONL/RESET：约 182.723 秒内 2510/2510 次 Modbus 请求成功，2510 条 telemetry 有效；
  运行中人工复位 NUCLEO 后继续读取，串口只打开一次并有界停止；
- 本地 MQTT：修正一个不合约的 MQTT client ID 后，约 82.233 秒内 1140/1140 次 Modbus
  请求成功，发布成功 1143、失败 0；本机订阅器收到 1140 条消息并覆盖 17 个 topic；
- 双总线：约 208.691 秒内 2860/2860 次 Modbus 请求成功，六种 CAN 周期 ID 各收到 213 帧，
  最大周期帧间隔约 0.983 秒；专用错误帧为 0，CAN warning/passive/bus-off 和 RX drop 增量
  均为 0，Host CAN TX 为 0。

MQTT 首次尝试使用了超过项目三约束且含连字符的 client ID，网关在启动准入阶段拒绝运行；改为
合约内的 `p5p3rpi0822` 后通过。该次失败属于配置准入，不属于 RS485、STM32 或 MQTT 传输故障。

## 6. 后续复验的项目五固件准入

后续再次进行项目三与项目五实物联调时，优先直接使用项目五当前已恢复并通过回归的默认固件，
不因“开始联调”本身重复烧写。项目五正式长稳收尾记录的默认 Debug ELF SHA-256 为：

```text
45b4a2ed3ee64ade9be820c1bf917632e45e70ac32ff2d7759335bff83d02e1d
```

该默认固件已经提供项目三当前联调范围所需的固定地址 4、`19200 8E1` Modbus RTU 只读输入
寄存器映射、三传感源与健康状态，以及 500 kbit/s 经典 CAN 周期遥测。树莓派或 Gateway 主机
只负责运行项目三、Modbus 轮询和 SocketCAN 观察，不承担 ST-LINK 烧录；确需重刷时仍在
Windows 使用项目五已记录的 STM32CubeProgrammer 流程完成。

每次联调开始前先执行最小准入：

1. NUCLEO RESET 后 VCP 只出现正常 BOOT、时钟摘要和五次 heartbeat；
2. 不出现 `P5DIAG1`、`P5ADXL1`、`P5CANACK1` 或 device-probe 摘要；
3. 新 CH340 路径在地址 4、`19200 8E1` 下至少完成一次只读请求，随后读取完整 122 个输入
   寄存器快照；
4. 需要双总线补充时，将 Host CAN 配置为 500 kbit/s、sample point `0.75`，先确认六类周期
   遥测 ID 持续可见；项目三产品范围仍为 RS485 主动轮询加 CAN 被动观察。

只有出现以下任一情况才重新烧写默认固件：当前板上仍是 probe、ADXL、CAN ACK、RX_ONLY、
TX_ONCE 或 `P5_SOAK_DIAGNOSTIC` 诊断变体；VCP 出现上述诊断标记；最小 Modbus/CAN 准入失败且
怀疑 Flash 身份不符；或者项目五寄存器映射、CAN 合同或正式源码已经发生需要联调消费的变更。
重新烧写后必须重复最小准入，不把“program/verify 成功”直接等同于项目三互操作通过。

该 SHA-256 是项目五正式 8 小时诊断固件完成后恢复的默认固件身份，不改写本报告第 1、5 节
历史联调实际使用的 `d076dd...` ELF。后续若项目五产生新的默认固件，应记录新提交、ELF
SHA-256 和合同变化，再决定项目三 profile 是否需要同步。

## 7. 保留边界

- 未连接商用 TAS-WS-R00020 或多个真实从站；
- Raspberry Pi 已完成本地 JSONL、一次 RESET、本地 MQTT 和“RS485 主动 + CAN 被动”短时补验，
  但项目三应用层尚未实现 CAN 数据消费；仍未执行生产 systemd 部署、远程/TLS MQTT、完整
  电气安全、隔离、EMC、重复断线或硬件长稳；
- Host→STM32 CAN 主动应用通信因 T1～T4 异常保持排除，不由本报告声明通过；
- 未执行地址 4→5→4 写入，项目五 H08/H09 继续为 `NOT_RUN_BY_POLICY`；
- 原始 JSONL/MQTT 日志在成功摘要形成后已删除，没有建立逐帧证据包；
- 项目三整体继续保持 `PUBLISHED=false`、`HARDWARE_VALIDATED=false`、`TAG=null`。

本报告只关闭项目五矩阵中窄范围的 `P3-01/HW-002`；项目三更广的 G6 硬件范围和项目五
`SOAK-02` 仍分别开放。
