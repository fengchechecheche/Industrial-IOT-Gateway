# Industrial-IOT-Gateway MQTT 消息合同

> 文档状态：`P3-S4-T01` 已审核固化  
> 合同版本：`1.0`  
> 适用协议：MQTT 3.1.1  
> 编制与批准日期：2026-08-09

## 1. 文档目的

本文档冻结 Industrial-IOT-Gateway 软件 MVP 的 MQTT 上行合同，包括 broker 边界、topic、
payload、QoS、MQTT RETAIN、Clean Session、Last Will and Testament（LWT）、重连退避和
离线积压策略。

本文档是 `P3-S4-T02` 实现与 `P3-S4-T03` 故障验收的输入，不表示 MQTT 客户端、本地
broker 或断线恢复已经实现或通过。

## 2. 约束来源

本合同必须同时满足：

1. `../2026-07-25至2026-07-28_工作任务产物/register_map.yaml`：16 个寄存器 topic、
   工程量和质量传播规则；
2. `../2026-07-25至2026-07-28_工作任务产物/architecture.md`：有界 `PublishQueue`、
   MQTT 不阻塞串口、2000 ms MQTT drain 和 5000 ms 总关闭预算；
3. `../2026-07-25至2026-07-28_工作任务产物/acceptance_protocol.md`：G3 MQTT 与故障门；
4. `mqtt_golden_payloads.json`（独立仓库维护位置为
   `tests/data/mqtt_golden_payloads.json`）：机器可读的固化黄金消息。

若这些基线与本文档冲突，停止受影响实现并先形成合同变更记录。

## 3. 范围与非范围

### 3.1 本合同负责

- 三类上行 topic：寄存器遥测、设备状态和网关健康；
- 四种寄存器质量：`fresh`、`stale`、`offline`、`invalid`；
- MQTT 3.1.1 的 QoS、RETAIN、Clean Session 和 LWT 参数；
- 应用有界积压、重连顺序、重复消息标识和关闭语义；
- 本地匿名 broker 的验收边界。

### 3.2 本合同不负责

- MQTT 下行写寄存器、远程配置或远程固件升级；
- TLS、用户名密码、ACL、证书轮换或公网 broker；
- MQTT 5.0 的 Session Expiry、Message Expiry、Reason String 或 User Property；
- 真实 RS485、STM32 或工业现场部署；
- 选择和安装 Eclipse Paho、Mosquitto 的具体软件包版本。

## 4. 术语

| 术语 | 本项目含义 |
|---|---|
| MQTT client | 运行在网关进程中的 MQTT 发布客户端 |
| broker | 接收发布并按订阅关系转发消息的服务器 |
| topic | MQTT 路由名称，不是文件路径或 JSON 字段 |
| payload | PUBLISH 报文承载的 UTF-8 JSON 字节 |
| MQTT RETAIN | broker 是否保存该 topic 的最后一条消息 |
| `value_is_retained` | payload 中的业务字段，表示是否沿用历史有效测量值 |
| Clean Session | MQTT 3.1.1 连接是否丢弃旧会话状态 |
| LWT | 客户端异常断开时由 broker 代发的遗嘱消息 |
| `run_id` | 一次网关进程运行实例的 UUID |
| `sequence` | 一次运行内分配给消息的事件标识 |

MQTT RETAIN 与 `value_is_retained` 完全独立。前者控制 broker 存储，后者描述测量值来源。

## 5. Broker 与连接合同

| 参数 | MVP 冻结值 |
|---|---|
| MQTT 版本 | 3.1.1 |
| 开发 broker URI | `tcp://127.0.0.1:1883` |
| 网络暴露 | 仅允许回环地址 |
| 认证 | 本地验收匿名连接 |
| Client ID | 配置显式提供，匹配 `[A-Za-z0-9]{1,23}` 且在 broker 内唯一 |
| 示例 Client ID | `iiotgwLab01` |
| Clean Session | `true` |
| Keep Alive | 20 秒 |
| 连接超时 | 5 秒 |
| 自动订阅 | 无；MVP 只上行发布 |
| LWT QoS | 1 |
| LWT RETAIN | `true` |

匿名连接只允许用于隔离的本地软件验收。不得把监听地址改成 `0.0.0.0` 后继续使用匿名模式，
也不得据此声称生产安全能力已经具备。

## 6. Topic 合同

### 6.1 Topic 模板

| 消息类别 | Topic 模板 | QoS | MQTT RETAIN |
|---|---|---:|---|
| 寄存器遥测 | `industrial_iot_gateway/devices/{device_name}/registers/{register_name}` | 1 | false |
| 设备状态 | `industrial_iot_gateway/devices/{device_name}/status` | 1 | true |
| 网关健康 | `industrial_iot_gateway/gateways/{gateway_id}/health` | 1 | true |
| LWT | 与网关健康 topic 相同 | 1 | true |

发布时的 Topic Name 不得包含 `+` 或 `#`。topic 区分大小写，当前合同统一使用小写
ASCII、数字和下划线。

### 6.2 冻结的 16 个遥测 Topic

| 从站 | 设备 | 寄存器 | Topic |
|---:|---|---|---|
| 1 | `environment_sensor` | `ambient_temperature_c` | `industrial_iot_gateway/devices/environment_sensor/registers/ambient_temperature_c` |
| 1 | `environment_sensor` | `relative_humidity_pct` | `industrial_iot_gateway/devices/environment_sensor/registers/relative_humidity_pct` |
| 1 | `environment_sensor` | `sensor_status` | `industrial_iot_gateway/devices/environment_sensor/registers/sensor_status` |
| 1 | `environment_sensor` | `air_pressure_kpa` | `industrial_iot_gateway/devices/environment_sensor/registers/air_pressure_kpa` |
| 1 | `environment_sensor` | `sample_period_ms` | `industrial_iot_gateway/devices/environment_sensor/registers/sample_period_ms` |
| 2 | `motor_actuator` | `target_speed_rpm` | `industrial_iot_gateway/devices/motor_actuator/registers/target_speed_rpm` |
| 2 | `motor_actuator` | `enable_command` | `industrial_iot_gateway/devices/motor_actuator/registers/enable_command` |
| 2 | `motor_actuator` | `actual_speed_rpm` | `industrial_iot_gateway/devices/motor_actuator/registers/actual_speed_rpm` |
| 2 | `motor_actuator` | `motor_current_a` | `industrial_iot_gateway/devices/motor_actuator/registers/motor_current_a` |
| 2 | `motor_actuator` | `actuator_fault_flags` | `industrial_iot_gateway/devices/motor_actuator/registers/actuator_fault_flags` |
| 3 | `fault_injection_device` | `fault_mode` | `industrial_iot_gateway/devices/fault_injection_device/registers/fault_mode` |
| 3 | `fault_injection_device` | `response_delay_ms` | `industrial_iot_gateway/devices/fault_injection_device/registers/response_delay_ms` |
| 3 | `fault_injection_device` | `exception_code` | `industrial_iot_gateway/devices/fault_injection_device/registers/exception_code` |
| 3 | `fault_injection_device` | `request_count` | `industrial_iot_gateway/devices/fault_injection_device/registers/request_count` |
| 3 | `fault_injection_device` | `fault_injection_count` | `industrial_iot_gateway/devices/fault_injection_device/registers/fault_injection_count` |
| 3 | `fault_injection_device` | `uptime_seconds` | `industrial_iot_gateway/devices/fault_injection_device/registers/uptime_seconds` |

设备状态 topic 由三个冻结设备名分别生成。网关健康 topic 使用公开示例
`gateway_id=lab_gateway_01` 时为：

```text
industrial_iot_gateway/gateways/lab_gateway_01/health
```

## 7. JSON 编码规则

- payload 必须是一个 UTF-8 JSON object，不允许顶层数组；
- JSON 字段名使用 `snake_case`；
- 时间戳使用 UTC RFC 3339 格式并保留毫秒，例如 `2026-08-09T10:00:00.123Z`；
- `sequence` 是 JSON 非负整数，不使用字符串表示；
- JSON 不允许 `NaN`、`Infinity` 或 `-Infinity`；
- 未定义的可选值应省略；只有合同明确要求“字段存在但当前无值”时使用 `null`；
- 生产代码使用 JSON 库序列化，不手工拼接或转义字符串。

## 8. 公共 Payload 字段

所有消息都必须包含：

| 字段 | 类型 | 规则 |
|---|---|---|
| `schema_version` | string | 当前固定为 `1.0` |
| `message_type` | string | `telemetry`、`device_status` 或 `gateway_health` |
| `gateway_id` | string | 当前网关逻辑 ID；示例为 `lab_gateway_01` |
| `run_id` | string | 进程启动时生成一次的 UUID v4，同一进程不变 |
| `sequence` | integer | 本次运行内分配；同一消息重发必须保持不变 |
| `source_timestamp` | string | 业务事件发生或被网关观测的时间 |
| `gateway_timestamp` | string | payload 生成并提交发布队列的时间 |

`run_id + sequence` 用于识别 QoS 1 重复消息。由于网络重排、队列合并和 LWT 延迟发布，
订阅端不得仅因为新收到的 `sequence` 小于上一条就直接丢弃消息。`sequence` 不是所有 topic
之间严格到达有序的承诺。

### 8.1 双时间戳语义

当前 Modbus 从站不提供自己的采样时钟，因此：

- 遥测的 `source_timestamp` 是网关确认完整有效 RTU 响应时的系统时钟，属于采样时间代理；
- 质量或设备状态的 `source_timestamp` 是状态转换被检测到的时间；
- `gateway_timestamp` 是 MQTT payload 被构造并进入发布路径的时间；
- LWT payload 在建立连接前固定，两个时间戳表示 LWT 注册时间，不是实际断线时间。

### 8.2 字段追踪矩阵

| 字段 | 权威来源 | T02 预期生成者 | 黄金场景 |
|---|---|---|---|
| `schema_version` | 本合同 | MQTT serializer | 全部 |
| `message_type` | 本合同 | 领域消息到 MQTT envelope 的适配层 | 全部 |
| `gateway_id` | MQTT 公开示例配置 | 应用启动配置 | 全部 |
| `run_id` | 本合同 | 进程启动阶段 | 全部 |
| `sequence` | 本合同 | 发布领域事件生成器 | 全部 |
| `source_timestamp` | 架构时间边界与本合同 8.1 | RTU 接收、状态转换或 LWT 注册点 | 全部 |
| `gateway_timestamp` | 本合同 8.1 | MQTT payload 构造点 | 全部 |
| `device_name/slave_id/register_name` | `register_map.yaml` | runtime config + quality worker | 遥测、设备状态 |
| `raw_value/engineering_value/unit` | `register_map.yaml` | decoder + quality worker | `TLM_*` |
| `quality/value_is_retained` | `register_map.yaml` 质量合同 | `FreshnessTracker` + quality worker | `TLM_*` |
| topic、QoS、MQTT RETAIN | 本合同第 6、12 节 | `gateway_mqtt` | 全部 |
| `state/reason/lwt` | scheduler 状态机与本合同第 10、11 节 | 状态适配器、生命周期和 LWT builder | 状态、健康、LWT |

## 9. 遥测 Payload

遥测消息除公共字段外还必须包含：

| 字段 | 类型 | 规则 |
|---|---|---|
| `device_name` | string | 与寄存器表一致 |
| `slave_id` | integer | `1..247` |
| `register_name` | string | 与寄存器表一致 |
| `raw_value` | integer、number 或 null | 缩放前解码值；没有任何样本时为 null |
| `engineering_value` | number | 条件字段；见质量规则 |
| `unit` | string | 与寄存器表一致 |
| `quality` | string | `fresh/stale/offline/invalid` |
| `quality_reason` | string | 产生当前质量的结构化原因 |
| `value_is_retained` | boolean | 是否沿用历史有效值 |

整数类型的 `raw_value` 是按冻结字节序/字序解码后的整数、尚未应用 `scale` 和 `offset`；
`float32` 的 `raw_value` 是按冻结 IEEE 754 和字序解码后的 JSON number。若以后需要保存线上
两个 16 位寄存器原词，应新增独立字段并升级 schema，不得改变 `raw_value` 既有含义。

### 9.1 质量传播

| 质量 | 值来源 | `raw_value` | `engineering_value` | `value_is_retained` |
|---|---|---|---|---|
| `fresh` | 当前有效响应 | 当前值 | 必须存在 | false |
| `stale` | 最后有效样本 | 历史值；无历史时 null | 有历史时存在 | 有历史时 true |
| `offline` | 最后有效样本 | 历史值；无历史时 null | 有历史时存在 | 有历史时 true |
| `invalid` | 当前无效响应 | 可用原始值；没有时 null | 必须省略 | false |

`quality_reason` 冻结值为：

- `valid_sample`
- `freshness_expired`
- `poll_failure`
- `decode_error`
- `invalid_raw_value`
- `device_offline`
- `device_probing`
- `device_recovered`
- `no_valid_sample`

增加新原因不改变质量状态机，但必须更新黄金向量和消费者文档。

## 10. 设备状态 Payload

设备状态 topic 发布设备级状态，字段包括：

| 字段 | 类型 | 规则 |
|---|---|---|
| `device_name` | string | 冻结设备名 |
| `slave_id` | integer | 对应 Modbus 从站地址 |
| `state` | string | `online/offline/probing` |
| `reason` | string | 状态转换原因 |

设备状态消息使用 QoS 1 和 MQTT RETAIN。新订阅者可以立即得到 broker 保存的最近状态。

## 11. 网关健康与 LWT Payload

网关健康字段包括：

| 字段 | 类型 | 规则 |
|---|---|---|
| `state` | string | `online/stopping/offline` |
| `reason` | string | `started/graceful_shutdown/unexpected_disconnect` |
| `lwt` | boolean | 是否由 broker 代发 LWT |

连接成功后应用发布 retained `online`。正常关闭时先发布 retained `stopping`，处理有界 drain，
再发布 retained `offline`；收到 PUBACK 后发送 MQTT DISCONNECT。

LWT 在每次 CONNECT 前注册为 retained `offline`、`reason=unexpected_disconnect`、
`lwt=true`。如果正常 `offline` 在关闭预算内未获确认，T02 必须记录 `drain_expired`，不得
静默报告优雅关闭成功。

## 12. QoS、RETAIN 与重复消息

所有消息使用 QoS 1，即“至少一次”投递。发送方得到 PUBACK 才能判定该次发布完成，但网络
中断可能导致同一应用消息再次到达，因此：

- 同一应用消息重发时保持相同 `run_id`、`sequence`、topic 和 payload；
- 消费者用 `run_id + sequence` 做幂等去重；
- 不把 QoS 1 描述为“恰好一次”；
- 遥测不使用 MQTT RETAIN，避免新订阅者把历史测量误当作实时采样；
- 状态和健康使用 MQTT RETAIN，使新订阅者获得最近状态。

## 13. Clean Session 与应用积压

连接固定 `CleanSession=true`。broker 不负责跨连接保存网关的会话积压；网关使用自身有界
发布队列：

- 容量沿用 `PublishQueue=1024`，高水位为 820；
- 同一 topic 的普通遥测可以合并为最新项；
- 质量转换、设备状态和网关健康不得静默丢失；
- broker 断线不得阻塞串口线程、scheduler 或质量线程；
- 队列满必须产生结构化指标和限频日志。

重连后按以下顺序恢复：

1. 确认 CONNACK；
2. 发布 retained 网关 `online`；
3. 发布三个设备当前状态快照；
4. 检查离线积压的遥测；
5. 丢弃已经超过 freshness 边界的 queued `fresh` 消息；
6. 由当前质量快照发布 `stale/offline/invalid`，不得刷新旧样本的 `source_timestamp`；
7. 继续处理新遥测。

## 14. 重连退避

应用管理重连，名义退避序列为：

```text
1 s, 2 s, 4 s, 8 s, 16 s, 30 s, 30 s, ...
```

每次实际等待在名义值的 `[0.8, 1.2]` 范围内加入抖动；测试必须注入可控抖动源，不能依赖
真实随机时间。成功收到 CONNACK 后重置为首档。每次连接失败、计划重连、实际等待和成功
恢复都产生指标和结构化事件。

## 15. 关闭合同

- MQTT drain 上限：2000 ms；
- 网关总优雅关闭上限：5000 ms；
- 关闭期间不接受新的普通遥测；
- 已接受消息在 drain 预算内尽力获得 PUBACK；
- 超时后记录未确认数量和 `drain_expired`；
- 不允许无限等待 broker；
- MQTT client 只释放一次，MQTT 线程必须 join。

## 16. 安全边界

- 本地匿名 broker 必须只监听 `127.0.0.1`；
- 仓库、合同和教程不得保存 MQTT 密码、私钥、Wi-Fi 密码或生产 broker 地址；
- MQTT 下行控制保持关闭；
- 不允许通过 topic 拼接注入 `+`、`#`、空层级或大小写变体；
- 日志不得打印凭据；
- 本合同通过不表示 TLS、认证、授权或生产部署通过。

## 17. T02 公共接口移交

T02 应按本合同补充领域字段，但第三方 Paho 类型不得进入 `gateway_core` 公共头文件：

1. `TelemetryEvent` 补齐原始值、工程量、单位、双时间戳、`run_id` 和 `sequence`；
2. 设备状态和网关健康成为明确的发布消息类型；
3. `GatewayRuntime` 依赖抽象 publish sink；
4. JSONL evidence sink 保留为测试实现；
5. `gateway_mqtt` 独立实现 JSON 序列化、Paho 适配、重连和 MQTT 指标；
6. `GATEWAY_ENABLE_MQTT=OFF` 时非 MQTT 构建与 PTY 测试继续成立。

## 18. 固化验收清单

- [x] 16 个冻结遥测 topic 与 `register_map.yaml` 一致且不重复
- [x] 三类 topic 的 QoS 和 MQTT RETAIN 无歧义
- [x] 四种质量的字段存在性和 retained 语义无歧义
- [x] Clean Session、LWT、重复消息和重连顺序无歧义
- [x] 本地匿名 broker 与生产安全能力边界明确
- [x] `mqtt_golden_payloads.json` 通过 JSON 语法和合同检查
- [x] 未把 T01 合同写成 T02 已实现能力
- [x] 审核者已于 2026-08-09 批准固化
