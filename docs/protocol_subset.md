# industrial_iot_gateway Modbus RTU 协议子集

## 1. 文档状态

| 字段         | 内容                                                     |
| ------------ | -------------------------------------------------------- |
| 项目         | 项目三：工业通信网关                                     |
| 任务编号     | `P3-S1-T02`                                              |
| 文档状态     | 已固化（S1 验收通过）                                    |
| 文档日期     | 2026-08-08                                               |
| 目标产物     | `2026-07-25至2026-07-28_工作任务产物/protocol_subset.md` |
| 实现状态     | 未开始                                                   |
| 测试状态     | 未开始                                                   |
| 后续实施入口 | `P3-S2-T02` 至 `P3-S2-T05`                               |

本文档冻结 `industrial_iot_gateway` 软件 MVP 使用的 Modbus RTU 协议子集、帧格式、地址语义、错误分类和黄金测试向量。

本文档只定义协议合同，不表示 CRC、编解码器、流式 parser、PTY 从站或真实 RS485 通信已经实现或验证。

## 2. 规范依据

本协议子集依据：

1. [MODBUS Application Protocol Specification V1.1b3](https://www.modbus.org/file/secure/modbusprotocolspecification.pdf)
2. [MODBUS over Serial Line Specification and Implementation Guide V1.02](https://www.modbus.org/file/secure/modbusoverserial.pdf)

术语约定：

- 应用协议层使用 client/server；
- 串行链路层沿用 master/slave；
- 本项目 Linux/C++ 网关是 RTU master/client；
- PTY 模拟器、商用设备和 STM32 是 RTU slave/server。

本文档定义的是经过裁剪的项目子集，不宣称实现完整 Modbus 功能集，也不宣称已经通过任何 Modbus 一致性认证。

## 3. 协议参与者

| 参与者                | Modbus 角色       | 当前阶段      | 职责                         |
| --------------------- | ----------------- | ------------- | ---------------------------- |
| Linux/C++ 网关        | RTU master/client | 软件 MVP 主体 | 主动发起请求并处理响应       |
| PTY 从站模拟器        | RTU slave/server  | 软件 MVP 必需 | 自动化集成、黄金帧和故障注入 |
| 商用 RS485 从站       | RTU slave/server  | 后续硬件增强  | 第三方互操作验证             |
| STM32 自研从站        | RTU slave/server  | 条件执行      | 寄存器、故障注入和固件验证   |
| Modbus Poll           | RTU master/client | 对照工具      | 单独测试实物从站             |
| Modbus Slave/PyModbus | RTU slave/server  | 对照工具      | 第三方软件从站               |

同一条 RS485 总线上同时只允许一个活动主站。使用 Modbus Poll 时必须停止 `industrial_iot_gateway` 主程序。

从站不得主动发送未被请求的数据，从站之间不得直接通信。

## 4. MVP 支持范围

### 4.1 支持的功能码

| 功能码                     | 名称                   | 用途             |
| -------------------------- | ---------------------- | ---------------- |
| `0x03`                     | Read Holding Registers | 读取保持寄存器   |
| `0x04`                     | Read Input Registers   | 读取输入寄存器   |
| `0x06`                     | Write Single Register  | 写单个保持寄存器 |
| `request_function \| 0x80` | Exception Response     | 从站异常响应     |

### 4.2 明确不支持的内容

软件 MVP 不支持：

- Modbus ASCII；
- Modbus TCP；
- 地址 `0` 广播；
- `0x01`、`0x02`、`0x05`；
- `0x0F`、`0x10` 多点写入；
- 文件记录、诊断和设备标识功能码；
- 用户自定义功能码；
- 一个请求跨越多个不连续寄存器区间；
- 同一串行总线存在多个在途请求；
- 未经配置的自动波特率或自动校验位探测。

Modbus 标准允许地址 `0` 用于不返回响应的写广播，但本项目 MVP 主站明确拒绝生成广播请求。该限制是项目裁剪，不得表述为 Modbus 标准本身不支持广播。

## 5. 串口和 RTU 模式

### 5.1 默认串口参数

项目默认参数：

```text
mode: RTU
baud_rate: 19200
data_bits: 8
parity: even
stop_bits: 1
```

即默认使用 `19200 8E1`。

MVP 支持配置以下标准波特率：

```text
9600
19200
38400
115200
```

约束：

- 默认波特率为 `19200`，`9600`、`38400` 和 `115200` 只能通过明确配置启用，不支持非标准波特率 `119200`；
- 所有总线节点必须使用相同串口参数；
- 波特率、校验位和停止位必须来自明确配置；
- 不允许连接失败后静默轮换不同串口参数；
- 实际使用的波特率、校验位和停止位必须记录在测试证据中；
- 允许按设备手册配置 `8N1`，但必须记录为设备兼容设置，不能写成项目标准默认值；
- 使用无校验模式时，协议标准推荐采用两个停止位以保持 11 位字符格式；
- 实际硬件参数必须记录在对应测试证据中。

### 5.2 字符和帧时序

对于不高于 `19200 bit/s` 的波特率：

```text
Tchar = 每字符实际传输位数 / baud_rate
t1.5  = 1.5 × Tchar
t3.5  = 3.5 × Tchar
```

默认 `8E1` 每字符为 11 位。

对于高于 `19200 bit/s` 的波特率，项目采用官方推荐固定值：

```text
t1.5 = 750 μs
t3.5 = 1.750 ms
```

规则：

- 相邻 RTU 帧之间至少保持 `t3.5` 静默；
- 帧内字符间静默超过 `t1.5` 时，当前帧判定为不完整；
- 不完整帧必须丢弃并记录错误；
- 一次操作系统 `read()` 不等于一个完整 Modbus 帧；
- response timeout 是调度配置，不由 Modbus 应用协议固定；
- response timeout 必须大于请求传输、从站处理、响应传输及帧间隔的总预算。

## 6. RTU ADU 格式

```text
+---------------+---------------+----------------+----------+
| Slave Address | Function Code | Data           | CRC16    |
| 1 byte        | 1 byte        | 0..252 bytes   | 2 bytes  |
+---------------+---------------+----------------+----------+
```

约束：

- RTU ADU 最大长度：`256 bytes`；
- Slave Address：1 字节；
- Function Code：1 字节；
- Data：最多 252 字节；
- CRC：2 字节；
- CRC 低字节先发送，高字节后发送；
- 所有 PDU 多字节字段按高字节在前编码；
- CRC 不包含串口起始位、停止位或校验位；
- CRC 计算范围从 Slave Address 开始，到 Data 最后一个字节结束。

## 7. 地址和寄存器语义

### 7.1 从站地址

| 地址范围  | 项目行为                   |
| --------- | -------------------------- |
| `0`       | 广播地址；MVP 主站拒绝发送 |
| `1–247`   | 合法单播从站地址           |
| `248–255` | 拒绝作为项目从站地址       |

主站 API 必须在序列化前拒绝非法地址。

从站收到不属于自己的单播地址时不返回响应。

### 7.2 寄存器地址

PDU 中的寄存器地址统一使用零基地址：

```text
PDU address 0     → 第一个寄存器
PDU address 1     → 第二个寄存器
PDU address 107   → 第 108 个寄存器
```

项目配置、C++ API 和自动化测试均使用 `0x0000–0xFFFF` 零基地址。

`30001`、`40001` 等表示方式只能作为人类阅读的 `display_reference`，不得参与线路编码或隐式偏移计算。

### 7.3 地址范围检查

所有范围计算必须使用宽于 16 位的中间类型：

```text
end_address = start_address + quantity - 1
```

只有以下条件全部满足时范围才合法：

```text
quantity >= 1
start_address <= 0xFFFF
end_address <= 0xFFFF
```

不得因 16 位整数回绕而把越界请求判定为合法。

## 8. 功能码合同

### 8.1 `0x03`：读取保持寄存器

请求 PDU：

```text
Function Code       1 byte   0x03
Starting Address    2 bytes  0x0000..0xFFFF
Quantity            2 bytes  1..125
```

完整 RTU 请求固定为 8 字节：

```text
Address Function StartHi StartLo QuantityHi QuantityLo CrcLo CrcHi
```

正常响应：

```text
Function Code       1 byte   0x03
Byte Count          1 byte   2 × Quantity
Register Values     N × 2 bytes
```

规则：

- 每个寄存器先发送高字节，再发送低字节；
- `Byte Count` 必须等于请求数量的两倍；
- 响应 ADU 长度必须等于 `5 + Byte Count`；
- 数量 `0` 或大于 `125` 必须在主站发送前拒绝；
- 起始地址和数量组成的范围必须合法；
- 主站必须核对响应从站地址和功能码。

### 8.2 `0x04`：读取输入寄存器

请求 PDU：

```text
Function Code       1 byte   0x04
Starting Address    2 bytes  0x0000..0xFFFF
Quantity            2 bytes  1..125
```

正常响应：

```text
Function Code       1 byte   0x04
Byte Count          1 byte   2 × Quantity
Register Values     N × 2 bytes
```

除寄存器类型和功能码外，其地址、数量、字节数及响应匹配规则与 `0x03` 相同。

`0x04` 只读取输入寄存器，不允许通过 `0x06` 写入。

### 8.3 `0x06`：写单个保持寄存器

请求 PDU：

```text
Function Code       1 byte   0x06
Register Address    2 bytes  0x0000..0xFFFF
Register Value      2 bytes  0x0000..0xFFFF
```

正常响应必须逐字节回显请求 PDU：

```text
Function Code       1 byte   0x06
Register Address    2 bytes
Register Value      2 bytes
```

完整请求和正常响应均固定为 8 字节。

规则：

- 协议层允许完整 `uint16` 数值范围；
- 寄存器必须在 `register_map.yaml` 中声明为可写；
- 写不存在或不允许写入的寄存器时，从站返回 `0x02`；
- malformed PDU、非法长度等协议结构问题返回 `0x03`；
- 内部写入失败返回 `0x04`；
- 正常响应的地址和值必须与请求完全一致；
- 回显内容不一致时，主站不得报告写入成功。

应用层数值上下限及其拒绝行为由后续 `register_map.yaml` 冻结，不在本文档中臆造。

## 9. 异常响应

异常响应格式：

```text
Slave Address
Request Function | 0x80
Exception Code
CRC Low
CRC High
```

异常响应 ADU 固定为 5 字节。

MVP 必须识别：

| 异常码 | 名称                  | 项目解释                           |
| ------ | --------------------- | ---------------------------------- |
| `0x01` | Illegal Function      | 功能码不受从站支持                 |
| `0x02` | Illegal Data Address  | 地址或地址范围对该从站无效         |
| `0x03` | Illegal Data Value    | 请求结构、数量或字段值在协议层非法 |
| `0x04` | Server Device Failure | 从站执行请求时发生不可恢复错误     |

处理规则：

- 异常功能码必须等于请求功能码按位或 `0x80`；
- 主站保留原始异常码，不把异常响应归类为通信超时；
- 未识别的异常码仍应保留原始字节，并归类为未知远程异常；
- 异常响应不得触发 parser 崩溃或越界；
- 是否重试由调度器策略决定，不由 codec 自动决定。

CRC、奇偶校验或帧格式错误属于通信错误。按照 Modbus 规则，从站不应对此返回异常响应；主站最终通过超时和错误计数处理。

## 10. 请求与响应匹配

每个响应必须与唯一在途请求匹配。

共同条件：

1. 响应从站地址等于请求从站地址；
2. 功能码等于请求功能码，或等于请求功能码 `| 0x80`；
3. CRC 正确；
4. 帧长度符合对应功能码；
5. 不存在未解释的尾随字节。

读取响应还必须满足：

```text
byte_count == requested_quantity × 2
received_data_bytes == byte_count
```

`0x06` 响应还必须满足：

```text
response_address == request_address
response_value == request_value
```

来自其他从站、其他功能码或已超时请求的晚到响应不得错误匹配到当前请求。

## 11. 流式 parser 合同

Parser 接收任意长度的字节片段，不假设一次 `read()` 对应一个 RTU 帧。

必须支持：

- 一个帧被拆成多次读取；
- 一次读取中包含多个完整帧；
- 帧前噪声；
- 坏 CRC；
- 错误长度；
- 截断帧；
- 超长帧；
- 异常响应；
- 正常帧后紧接下一帧；
- 超时后的晚到响应。

### 11.1 长度判定

候选长度规则：

| 响应类型        |    预期 ADU 长度 |
| --------------- | ---------------: |
| `0x03` 正常响应 | `5 + byte_count` |
| `0x04` 正常响应 | `5 + byte_count` |
| `0x06` 正常响应 |              `8` |
| 异常响应        |              `5` |

### 11.2 重新同步

由于 RTU 没有固定起始字节，重新同步必须是有界操作：

1. 优先使用当前在途请求的从站地址和预期功能码；
2. 根据功能码推导候选长度；
3. 校验候选 CRC；
4. 只接受与当前请求匹配的合法帧；
5. 噪声扫描不得超过接收缓冲区容量；
6. 在 `t3.5` 帧边界后仍无法得到合法帧时，清空当前坏帧；
7. 不允许无限增长缓冲区或无限逐字节扫描。

随机噪声中偶然出现合法 CRC 时，仍必须通过从站、功能码、长度及请求上下文检查。

## 12. 结构化错误分类

协议层至少输出以下错误类别：

```text
INVALID_SLAVE_ADDRESS
BROADCAST_UNSUPPORTED
UNSUPPORTED_FUNCTION
INVALID_QUANTITY
ADDRESS_RANGE_OVERFLOW
FRAME_TOO_SHORT
FRAME_TOO_LONG
INTER_CHARACTER_TIMEOUT
CRC_MISMATCH
BYTE_COUNT_MISMATCH
UNEXPECTED_SLAVE
UNEXPECTED_FUNCTION
RESPONSE_ECHO_MISMATCH
TRAILING_BYTES
REMOTE_EXCEPTION
UNKNOWN_EXCEPTION_CODE
NO_RESPONSE
```

每条错误至少携带：

```text
request_id
slave_id
expected_function
received_function
frame_hex
error_category
detail
timestamp
```

不得仅记录“通信失败”而丢失具体原因。

## 13. CRC16 合同

算法参数：

```text
width: 16
initial_value: 0xFFFF
reflected_polynomial: 0xA001
input_reflected: true
output_reflected: true
final_xor: 0x0000
wire_order: low byte, then high byte
```

计算范围：

```text
Slave Address + Function Code + Data
```

CRC 字段自身不参与 CRC 计算。

CRC API 只计算并返回 16 位数值，不负责串口字节序列化。序列化层负责把低字节放在线路前面。

CRC 错误帧不得进入功能码解码和寄存器更新流程。

## 14. 黄金测试向量

以下 CRC 在候选稿编写阶段独立计算，但尚未转化为仓库自动化测试，因此不代表 S2 已通过。

### 14.1 正常向量

| ID             | 语义                                | 完整 RTU 帧                  | 预期             |
| -------------- | ----------------------------------- | ---------------------------- | ---------------- |
| `G-03-REQ-01`  | 从站 1，从地址 0 读取两个保持寄存器 | `01 03 00 00 00 02 C4 0B`    | 解码成功         |
| `G-03-RSP-01`  | 返回值 10、20                       | `01 03 04 00 0A 00 14 DA 3E` | `[10, 20]`       |
| `G-04-REQ-01`  | 从站 1，从地址 0 读取一个输入寄存器 | `01 04 00 00 00 01 31 CA`    | 解码成功         |
| `G-04-RSP-01`  | 返回值 10                           | `01 04 02 00 0A 39 37`       | `[10]`           |
| `G-06-REQ-01`  | 向保持寄存器地址 1 写入 3           | `01 06 00 01 00 03 98 0B`    | 解码成功         |
| `G-06-RSP-01`  | `0x06` 正常回显                     | `01 06 00 01 00 03 98 0B`    | 与请求逐字节一致 |
| `G-03-MAX-QTY` | `0x03` 最大数量 125                 | `01 03 00 00 00 7D 85 EB`    | 合法请求         |

### 14.2 异常响应向量

| ID           | 语义              | 完整 RTU 帧      | 预期                    |
| ------------ | ----------------- | ---------------- | ----------------------- |
| `G-EX-03-02` | `0x03` 非法地址   | `01 83 02 C0 F1` | `REMOTE_EXCEPTION/0x02` |
| `G-EX-04-03` | `0x04` 非法数据值 | `01 84 03 03 01` | `REMOTE_EXCEPTION/0x03` |
| `G-EX-06-04` | `0x06` 设备故障   | `01 86 04 43 A3` | `REMOTE_EXCEPTION/0x04` |

### 14.3 拒绝向量

| ID                | 完整 RTU 帧                     | 预期                                   |
| ----------------- | ------------------------------- | -------------------------------------- |
| `R-QTY-ZERO`      | `01 03 00 00 00 00 45 CA`       | `INVALID_QUANTITY`                     |
| `R-ADDR-OVERFLOW` | `01 03 FF FF 00 02 C4 2F`       | `ADDRESS_RANGE_OVERFLOW`               |
| `R-BROADCAST`     | `00 06 00 01 00 03 99 DA`       | 主站发送前 `BROADCAST_UNSUPPORTED`     |
| `R-SLAVE-248`     | `F8 03 00 00 00 01 90 63`       | `INVALID_SLAVE_ADDRESS`                |
| `R-BAD-CRC`       | `01 03 00 00 00 02 C4 0A`       | `CRC_MISMATCH`                         |
| `R-TRUNCATED`     | `01 03 04 00 0A`                | 等待补帧；到达边界后 `FRAME_TOO_SHORT` |
| `R-BYTE-COUNT`    | `01 03 03 00 0A 00 43 2E`       | 针对数量为 2 的在途请求，返回 `BYTE_COUNT_MISMATCH` |
| `R-NOISE-PREFIX`  | `AA 55 01 03 00 00 00 02 C4 0B` | 丢弃噪声后恢复合法请求帧               |
| `R-CONCATENATED`  | `01 03 00 00 00 02 C4 0B 01 04 00 00 00 01 31 CA` | 输出两个独立帧事件 |

`R-BYTE-COUNT` 使用 `G-03-REQ-01` 作为在途请求上下文：请求数量为 2，正常 `byte_count` 应为 4；输入帧自身 CRC 正确，但声明的 `byte_count=3`，因此必须返回 `BYTE_COUNT_MISMATCH`。

`R-CONCATENATED` 由 `G-03-REQ-01` 和 `G-04-REQ-01` 两个完整合法帧逐字节拼接而成，parser 必须按顺序输出两个独立帧事件。表中所有声明为完整合法 ADU 的向量均提供完整字节序列和正确 CRC；`R-BAD-CRC`、`R-TRUNCATED` 与 `R-NOISE-PREFIX` 分别是故意损坏、截断或带噪输入流，不要求整个输入流具备单一有效 CRC。

## 15. 测试向量数据结构

S2 将黄金向量转成机器可读数据，建议字段：

```yaml
id: G-03-REQ-01
direction: request
slave_id: 1
function: 0x03
frame_hex: "01 03 00 00 00 02 C4 0B"
expected:
  status: ok
  start_address: 0
  quantity: 2
```

错误向量示例：

```yaml
id: R-QTY-ZERO
direction: request
frame_hex: "01 03 00 00 00 00 45 CA"
expected:
  status: error
  category: INVALID_QUANTITY
```

每条向量必须具备唯一 ID、完整帧、预期状态以及结构化预期值或错误类别。

## 16. 与后续文档的边界

### `register_map.yaml`

负责：

- 具体从站；
- 具体寄存器地址；
- 数据类型；
- 缩放和偏移；
- 单位；
- 读写属性；
- 轮询周期；
- 数据新鲜度；
- 应用层数值约束。

### `architecture.md`

负责：

- 串口所有权；
- 请求队列；
- 在途请求；
- timeout；
- retry；
- backoff；
- 关闭和线程唤醒；
- 缓冲区实际容量。

### `acceptance_protocol.md`

负责：

- 测试层级；
- 故障矩阵；
- 通过门限；
- 证据 schema；
- 长稳测试；
- 软件与硬件验收边界。

本文档不得提前替代上述工件。

## 17. S2 实施约束

S2 实现必须遵守：

1. 先把本文档黄金向量转为失败的自动化测试；
2. 再实现 CRC、codec 和 parser；
3. 不根据当前实现反向修改协议合同；
4. 如发现合同错误，先形成变更记录并重新审核；
5. CRC、codec、parser 和 scheduler 错误必须分层；
6. 不允许用第三方 Modbus 库替代项目核心实现；
7. 第三方库只能作为测试预言机或互操作对照；
8. 没有 PTY 证据时不得宣称软件集成通过；
9. 没有真实串口证据时不得宣称 RS485 硬件通过。

## 18. 文档验收清单

- [x] 协议角色无歧义
- [x] 支持和拒绝的功能码明确
- [x] 广播裁剪边界明确
- [x] 从站地址范围明确
- [x] 寄存器统一使用零基地址
- [x] `0x03` 和 `0x04` 数量限制明确
- [x] `0x06` 回显规则明确
- [x] RTU ADU 最大长度明确
- [x] CRC 参数和线路字节序明确
- [x] `t1.5`、`t3.5` 和 response timeout 边界明确
- [x] 异常响应和通信错误分离
- [x] 半帧、粘连帧、噪声和重新同步规则明确
- [x] 结构化错误类别明确
- [x] 所有声明为完整合法 ADU 的黄金向量均具备完整字节序列和正确 CRC
- [x] 未把协议合同写成已实现或已验证
- [x] 审核者已授权修复阻塞项并固化为 `P3-S1-T02` 基线

以上内容已通过审核，本文档自 2026-08-08 起固化为 `P3-S1-T02` 基线。该结论只表示“冻结 Modbus RTU 子集”工作块通过，不表示 CRC、codec、parser、PTY 集成、真实串口或其他 S2 实现与测试已经开始或通过。
