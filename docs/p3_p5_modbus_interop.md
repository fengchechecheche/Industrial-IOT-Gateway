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

125 秒正式并发轮次和随后 25 秒聚焦确认得到：

- 项目三在 CAN 周期遥测同时存在时完成 2060/2060 次 Modbus 请求，失败 0，覆盖全部
  17 个输入寄存器地址；
- 六种 CAN 周期 ID 持续出现；并发轮询期间发送四个不同 sequence/nonce 的 Host-owned
  `0x540` 请求，均收到唯一匹配的 STM32-owned `0x541` 应答；
- `can0` RX 从 738 增至 2228，Host TX 恰为 4；结束时仍为 ERROR-ACTIVE，bus-error、
  error-passive 和 bus-off 计数均为 0；
- 正式轮次出现一次 error-warning 状态转换，但未伴随 Modbus 失败或可见链路中断；随后
  25 秒内该计数保持为 1，没有再次增长。

因此可以声明“指定 WSL x86_64 Gateway 环境与项目五节点完成了约 150 秒真实 RS485/CAN
并发和四次有界 CAN 诊断往返”。不能改写为全程零 warning，也不能据此声明项目三应用层
已经消费、解析或发布 CAN 数据。成功原始 JSONL 在形成摘要后已删除。

## 5. 保留边界

- 本轮不是 Raspberry Pi 4B/ARM64 实物 RS485 验收；
- 未连接商用 TAS-WS-R00020 或多个真实从站；
- Gateway WSL 已完成 SocketCAN 实物补验，但项目三应用层尚未实现 CAN 数据消费；仍未执行
  Raspberry Pi CAN、完整电气安全、隔离、EMC、重复断线或硬件长稳；
- 未执行地址 4→5→4 写入，项目五 H08/H09 继续为 `NOT_RUN_BY_POLICY`；
- 原始 JSONL/MQTT 日志在成功摘要形成后已删除，没有建立逐帧证据包；
- 项目三整体继续保持 `PUBLISHED=false`、`HARDWARE_VALIDATED=false`、`TAG=null`。

本报告只关闭项目五矩阵中窄范围的 `P3-01/HW-002`；项目三更广的 G6 硬件范围和项目五
`SOAK-02` 仍分别开放。
