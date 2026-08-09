# P3-S4-T01｜MQTT 基础与消息合同

> 文档性质：面向初学者的原理与代码接口说明  
> 对应工作块：`P3-S4-T01`  
> 文档状态：已审核固化  
> 编制与批准日期：2026-08-09

## 1. 本工作块解决什么问题

S3 已经能够通过 Modbus RTU 轮询三个 PTY 从站，把寄存器值转换为工程量，并判断数据是
`fresh`、`stale`、`offline` 还是 `invalid`。但这些数据目前只写入本地 JSONL evidence
sink，外部程序还不能通过网络持续接收。

S4 将增加 MQTT 上行。MQTT 客户端、broker 和异步发布代码在 T02 才实现；T01 先回答：

- 数据发布到哪个 topic；
- JSON payload 有哪些字段；
- 消息是否允许重复、是否由 broker 保存；
- broker 断开时旧数据如何处理；
- 网关异常退出时订阅者如何知道它已经离线；
- 哪些功能明确不在 MVP 范围。

先冻结合同可以避免发布者和订阅者各自猜测字段含义，也能让后续测试先写出确定的期望值。

## 2. MQTT 在项目中的角色

本项目的两条主要通信链路用途不同：

```text
Modbus RTU / RS485                       MQTT / TCP

从站 ──寄存器响应──> 网关              网关 ──遥测与状态──> broker
          现场采集                                  │
                                                   ├──> 看板
                                                   ├──> 数据库采集器
                                                   └──> 诊断订阅者
```

Modbus RTU 是网关与现场从站之间的主从协议；MQTT 是网关把已采集数据上报给多个消费者的
发布/订阅协议。MVP 不通过 MQTT 接受写寄存器命令，因此 MQTT 故障不能改变 RTU 总线的
单在途请求规则，也不能阻塞串口轮询。

## 3. 发布者、订阅者和 Broker

MQTT 采用发布/订阅（publish/subscribe）模型：

- 发布者（publisher）把消息发给一个 topic；
- 订阅者（subscriber）声明感兴趣的 Topic Filter；
- broker 接收发布、执行 topic 匹配，再把消息转发给订阅者。

发布者通常不知道有多少订阅者，也不与每个看板或数据库建立专用业务连接。这样网关只需
维护到 broker 的一条 MQTT 连接。

例如网关发布：

```text
Topic:
industrial_iot_gateway/devices/environment_sensor/registers/ambient_temperature_c

Payload:
{"engineering_value":25.3,"quality":"fresh", ...}
```

订阅者可以订阅精确 topic，也可以使用 Topic Filter：

```text
industrial_iot_gateway/devices/+/registers/+
```

这里的 `+` 只允许出现在订阅过滤器中，表示匹配一个层级；发布使用的 Topic Name 不能包含
通配符。

## 4. Topic 和 Payload 的区别

Topic 是 broker 用来路由消息的名称；payload 是消息实际承载的字节。二者类似“信封上的
收件分类”和“信封里的正文”。

当前 topic：

```text
industrial_iot_gateway/devices/environment_sensor/registers/ambient_temperature_c
```

可以从层级上读出：

1. 项目命名空间 `industrial_iot_gateway`；
2. 资源类别 `devices`；
3. 设备 `environment_sensor`；
4. 子资源 `registers`；
5. 寄存器 `ambient_temperature_c`。

payload 则保存数值、质量、时间戳和事件标识。MQTT 本身不规定 payload 必须是 JSON，本项目
选择 JSON 是为了便于测试、日志比对和跨语言订阅。

## 5. MQTT 控制报文与应用 Payload

调用 MQTT 库发布 JSON 时，库会在 JSON 外面生成 MQTT PUBLISH 控制报文。控制报文包含：

- 报文类型；
- DUP、QoS 和 RETAIN 标志；
- Topic Name；
- QoS 1 使用的 Packet Identifier；
- payload 字节。

JSON 只位于 payload 部分。`sequence`、`quality` 和 `value_is_retained` 都是本项目定义的
应用字段，不是 MQTT 固有报文字段。

这也是为什么项目不自行实现 MQTT 报文编码，而计划在 `gateway_mqtt` 中使用 Eclipse Paho：
协议握手、Packet Identifier、PUBACK、Keep Alive 和网络细节由经过验证的库负责，项目代码
聚焦消息合同、有界缓存和业务质量。

## 6. QoS 0、1、2

Quality of Service（QoS）描述一次 MQTT 消息在单段发送者—接收者链路上的投递保证。

| QoS | 常用描述 | 确认过程 | 主要代价 |
|---:|---|---|---|
| 0 | 至多一次 | 没有发布确认 | 最低开销，允许丢失 |
| 1 | 至少一次 | PUBLISH → PUBACK | 可能重复 |
| 2 | 恰好一次 | 两阶段确认 | 状态和报文更多 |

本项目统一选择 QoS 1。原因是 16 个点位的数据量不大，遥测和设备状态需要明确的发布成功
确认，而 QoS 2 的额外交互对 MVP 没有必要。

“至少一次”意味着消息可能重复。例如 broker 已经收到 PUBLISH 并返回 PUBACK，但 PUBACK
在网络中丢失，客户端可能再次发送相同消息。因此 payload 使用：

```text
run_id + sequence
```

作为应用事件标识。重发同一消息时这两个字段不能改变。

`sequence` 不是跨 topic 的严格到达顺序。LWT 可能在注册很久后才发布，网络也可能让不同
topic 的消息以不同顺序到达。消费者应识别“是否处理过同一标识”，不应简单拒绝所有较小
sequence。

## 7. MQTT RETAIN 和历史值 Retained

这是本工作块最容易混淆的概念。

### 7.1 MQTT RETAIN

发布报文的 RETAIN 标志为 1 时，broker 保存该 topic 的最后一条消息。以后新订阅者建立订阅
时，可以立即收到这个最近状态，而不必等待发布者下一次更新。

本项目对设备状态和网关健康设置 MQTT RETAIN，因为新打开的监控程序需要立即知道当前
在线状态。

遥测不设置 MQTT RETAIN，避免新订阅者把几分钟前保存的温度直接理解为刚采集的数据。

### 7.2 `value_is_retained`

这是 JSON payload 中的业务字段。例如温度最后一次有效值是 25.3 °C，之后设备不响应：

```json
{
  "engineering_value": 25.3,
  "quality": "offline",
  "value_is_retained": true
}
```

这里表示 25.3 是历史有效值，不是当前成功读取的值。即使 MQTT PUBLISH 的 RETAIN 标志为
false，`value_is_retained` 仍然可以为 true。

| 场景 | MQTT RETAIN | `value_is_retained` |
|---|---:|---:|
| 当前 fresh 温度 | false | false |
| 沿用历史 stale 温度 | false | true |
| retained 设备 offline 状态 | true | 设备状态消息没有该字段 |

## 8. Clean Session

MQTT 3.1.1 的 Clean Session 控制 broker 是否跨网络连接保存 MQTT 会话状态。

本项目设置：

```text
CleanSession = true
```

每次连接都从干净会话开始，broker 不替网关保存跨连接积压。原因是网关已经有带质量规则的
有界 `PublishQueue`：

- 普通遥测可按 topic 合并；
- stale/offline 不得伪装为 fresh；
- 满队列必须计数；
- broker 断开不能拖住串口线程。

如果同时让 Paho、broker 和业务队列各保存一份离线消息，就难以判断恢复时应该先发哪一份，
也更容易重复发布旧 fresh 数据。

## 9. Last Will and Testament

Last Will and Testament（LWT）是客户端在 CONNECT 时交给 broker 的遗嘱消息。若连接因为
网络错误、Keep Alive 超时或未发送 DISCONNECT 而异常结束，broker 代客户端发布该消息。

本项目的 LWT：

```text
Topic: industrial_iot_gateway/gateways/lab_gateway_01/health
QoS: 1
MQTT RETAIN: true
state: offline
reason: unexpected_disconnect
lwt: true
```

正常关闭时，应用先发布 `stopping` 和 `offline`，再发送 DISCONNECT；broker 收到正常
DISCONNECT 后删除尚未触发的 LWT。

LWT payload 在建立连接前就已经固定，所以其中的时间戳表示“注册遗嘱的时间”，不是 broker
检测到断线的准确时间。MQTT 3.1.1 不会在代发时自动改写 JSON。

## 10. 双时间戳

遥测 payload 包含：

- `source_timestamp`：业务事件被观测的时间；
- `gateway_timestamp`：payload 被构造并进入发布路径的时间。

当前三个 Modbus 从站没有自己的时钟字段，所以 `source_timestamp` 不是传感器内部采样时间，
而是网关确认完整有效 RTU 响应时记录的系统时间。

断线缓存后重新发布时，不能把 `source_timestamp` 改成重连时间。否则历史数据看起来像新
采样。`gateway_timestamp` 可以反映消息何时进入 MQTT 路径，但它也不能替代原始采样代理。

网关内部的 timeout、退避和 freshness 仍使用 `std::chrono::steady_clock`；对外 RFC 3339
时间戳使用系统时钟。系统时钟可能被校时，不适合计算 deadline。

## 11. 四种数据质量

| 质量 | 含义 | 工程量规则 |
|---|---|---|
| `fresh` | 最近轮询成功且未超 freshness | 发布当前工程量 |
| `stale` | 有历史值，但已超 freshness | 可发布历史工程量，必须 retained |
| `offline` | 从站达到离线条件 | 可发布历史工程量，必须标记 offline/retained |
| `invalid` | 原始值无效或解码失败 | 省略 `engineering_value` |

例如环境温度寄存器的 raw 值为 253，scale 为 0.1：

```text
engineering_value = raw_value × scale + offset
                  = 253 × 0.1 + 0
                  = 25.3 degC
```

如果设备随后离线，25.3 可以保留，但质量必须变为 offline，不能继续标记 fresh。

## 12. Topic/Payload 如何映射到寄存器表

冻结寄存器定义：

```yaml
name: "ambient_temperature_c"
data_type: "int16"
scale: 0.1
unit: "degC"
mqtt:
  topic: "industrial_iot_gateway/devices/environment_sensor/registers/ambient_temperature_c"
```

对应 payload：

```json
{
  "device_name": "environment_sensor",
  "register_name": "ambient_temperature_c",
  "raw_value": 253,
  "engineering_value": 25.3,
  "unit": "degC",
  "quality": "fresh"
}
```

topic 不重复承载数值；payload 也不能自行创造与寄存器表不同的单位、scale 或名字。

## 13. Broker 断线与重连

名义重连退避为：

```text
1 s → 2 s → 4 s → 8 s → 16 s → 30 s → 30 s ...
```

每次加入 ±20% 抖动（jitter）。指数退避避免 broker 故障时客户端高频连接；抖动避免大量
网关在 broker 恢复瞬间同时重连。

broker 断线期间：

1. 串口轮询继续；
2. 普通遥测按 topic 合并；
3. 队列保持有界；
4. 质量状态继续根据时间变化；
5. 已超 freshness 的 queued fresh 不得在重连后继续按 fresh 发布。

重连成功后先发布网关和设备状态快照，再发布当前仍有意义的遥测。

## 14. 当前代码中的位置

T01 不修改运行时代码。当前实现为 T02 提供了以下接入点：

| 当前文件/类型 | 当前职责 | T02 需要补充 |
|---|---|---|
| `pipeline/pipeline_messages.hpp` 的 `TelemetryEvent` | topic、质量和工程量事件 | raw、单位、双时间戳、run ID、sequence |
| `pipeline/event_pipeline.hpp` 的 `PublishQueue` | 1024 容量的发布队列 | 继续承载 MQTT 前的有界背压 |
| `runtime/gateway_runtime.cpp` 的 `quality_loop` | 解码与质量转换 | 生成合同所需完整领域消息 |
| `runtime/gateway_runtime.cpp` 的 `sink_loop` | 当前消费队列并写 JSONL | 改为可替换 publish sink |
| 根 `CMakeLists.txt` | MQTT 选项当前拒绝启用 | 新增独立 `gateway_mqtt` 目标 |

### 14.1 当前执行过程

当前 S3 数据进入发布队列的过程是：

```text
serial_loop
  收到并解析 Modbus RTU 响应
      │
      ▼
MeasurementRaw
      │  push
      ▼
MeasurementQueue
      │
      ▼
quality_loop
  查找 RuntimeRegisterDefinition
  解码 raw registers
  应用 scale / offset
  更新 FreshnessTracker
      │
      ▼
TelemetryEvent / QualityTransitionEvent
      │  push
      ▼
PublishQueue
      │
      ▼
sink_loop
  转换为 StructuredEvent
  写入 JSONL
```

这个结构已经把慢速输出与串口线程隔开，但 `TelemetryEvent` 当前只保存 topic、quality 和一个
可选工程量，没有保存 MQTT 合同要求的 raw、unit、双时间戳、run ID 和 sequence。

### 14.2 T02 预计执行过程

T02 不会让 serial thread 直接调用 Paho，而是把末端替换为抽象 sink：

```text
PublishQueue
      │
      ▼
PublishSink interface
      ├── JsonlEvidenceSink（测试和证据）
      └── MqttPublishSink
              │
              ├── 领域消息转 JSON
              ├── 决定 QoS / MQTT RETAIN
              ├── 提交 mqtt::async_client
              └── 通过 callback 更新成功/失败指标
```

测试可以注入假 sink，不启动网络；MQTT 集成测试再使用真实本地 broker。这体现了依赖倒置：
核心运行时依赖项目自有接口，Paho 适配器依赖该接口，而不是让核心依赖第三方具体类型。

当前 JSONL evidence sink 不是 MQTT。只有 T02 接入本地 broker 并通过发布、断线和重连测试后，
才能声称 MQTT 已实现。

## 15. 为什么 `gateway_core` 不直接依赖 Paho 类型

协议、串口、调度和质量是网关核心能力。如果公共接口直接暴露 `mqtt::async_client`、
`mqtt::message` 等类型：

- 关闭 MQTT 后核心库仍需要 Paho 头文件；
- 单元测试很难替换假 publisher；
- 第三方 API 变化会传播到协议和调度模块；
- PTY 软件链路无法独立构建。

因此计划使用项目自有的领域消息和 publish sink 接口。`gateway_mqtt` 在边界处把领域消息
转换为 JSON 和 Paho 调用。

## 16. 常见错误与安全边界

### 16.1 把 QoS 1 当成恰好一次

QoS 1 允许重复，必须使用应用事件标识实现幂等。

### 16.2 把 MQTT RETAIN 当成数据新鲜

broker 保存的“最后消息”可能很旧。新鲜度必须读取 payload 的质量和时间戳。

### 16.3 重连后刷新旧数据时间戳

这会把历史值伪装成新值，属于静默数据错误。

### 16.4 在串口线程中等待 PUBACK

broker 变慢会让 Modbus 轮询停止，违反模块隔离。

### 16.5 匿名 broker 监听所有网卡

本项目只允许回环地址匿名验收。监听公网或局域网需要认证、授权和 TLS 设计。

### 16.6 通过 MQTT 直接写寄存器

MVP 禁止 MQTT 下行控制。以后若增加，必须单独设计 topic 白名单、鉴权、参数校验、审计和
总线公平性，不能复用上行 topic 直接执行。

## 17. 本工作块完成与未完成的能力

T01 固化完成后可以说明：

- topic、payload、QoS、RETAIN、Clean Session、LWT 和重连合同已经提出；
- 16 个寄存器 topic 和四种质量有黄金消息；
- T02 的代码接口和测试目标已经明确。

仍不能说明：

- 已安装或使用 Eclipse Paho；
- 已连接 Mosquitto；
- 已成功发布任何 MQTT 消息；
- broker 断线恢复已经通过；
- TLS、认证或生产部署已经完成。

## 18. 后续学习资料

以下均为一手资料，访问日期为 2026-08-09：

1. [OASIS MQTT 3.1.1 Specification](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html)  
   建议先读 3.1 CONNECT、3.3 PUBLISH、4.3 QoS、4.7 Topic。
2. [Eclipse Paho MQTT C++ Client](https://eclipse.dev/paho/clients/cpp/)  
   了解 C++ 客户端支持的 MQTT 版本、LWT、TLS、自动重连和非阻塞 API。
3. [Paho C++ `mqtt::async_client` Reference](https://eclipse.dev/paho/files/cppdoc/classmqtt_1_1async__client.html)  
   T02 阅读 `connect`、`publish`、`reconnect` 和 `set_callback`。
4. [Eclipse Mosquitto Documentation](https://mosquitto.org/documentation/)  
   了解 broker、客户端命令、监听器和认证配置。
5. [Mosquitto 2.0 Migration Notes](https://mosquitto.org/documentation/migrating-to-2-0/)  
   理解 Mosquitto 2.x 对远程监听与匿名访问默认值的调整。

学习时应把“MQTT 协议规定”“Paho 库接口”“本项目业务合同”分开：三者相互关联，但不是同一
层次。

## 19. 与后续工作块的关系

- T02：依据本合同实现异步 publisher、本地 broker、队列和重连；
- T03：停止 broker、制造慢消费者和队列压力，验证合同在故障下仍成立；
- T04：硬件仅改变 Modbus 数据来源，不改变 MQTT topic/payload；
- T05：把合同、测试和实际证据串成可追溯结论。
