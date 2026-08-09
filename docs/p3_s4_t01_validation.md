# P3-S4-T01 MQTT 合同固化验收记录

> 验收对象：MQTT topic/payload、断线策略、黄金消息和初学者教程  
> 验收日期：2026-08-09  
> 当前结论：`PASS`

## 1. 结论

`P3-S4-T01` 产物已生成、通过本地机器一致性检查并经用户审核批准。16 个遥测 topic 与冻结
`register_map.yaml` 完全一致；8 个黄金场景覆盖 fresh、stale、offline、invalid、无历史
offline、设备状态、网关在线和 LWT；QoS、MQTT RETAIN、`value_is_retained` 和 invalid
字段规则检查通过。

批准版本同步到独立仓库的 `docs/`、`docs/learning/` 和 `tests/data/`。该结论只证明 MQTT
合同、黄金消息和教学内容已冻结；T02 仍未开始，不能据此声称 MQTT 发布已经实现。

## 2. 固化产物

| 产物 | 用途 | 状态 |
|---|---|---|
| `mqtt_contract.md` | MQTT 3.1.1 topic/payload、连接、重连和关闭合同 | 已批准 |
| `mqtt_golden_payloads.json` | 机器可读黄金消息 | 已批准 |
| `p3_s4_t01_mqtt基础与消息合同.md` | 面向初学者的 MQTT 原理和代码接口教程 | 已批准 |
| `p3_s4_t01_validation.md` | 本验收记录 | 已批准 |

## 3. 已执行检查

| 检查 | 期望 | 实际 | 状态 |
|---|---:|---:|---|
| 核心产物存在 | 3 个核心产物 | 3 | PASS |
| 黄金 JSON 可解析 | 可解析 | 8 个 case | PASS |
| MQTT 协议版本 | 3.1.1 | 3.1.1 | PASS |
| 黄金消息 QoS | 全部为 1 | 8/8 | PASS |
| 遥测 MQTT RETAIN | 全部 false | 5/5 | PASS |
| 状态/健康 MQTT RETAIN | 全部 true | 3/3 | PASS |
| invalid 工程量 | 必须省略 | 已省略 | PASS |
| stale/offline 历史工程量 | `value_is_retained=true` | 满足 | PASS |
| 寄存器表遥测 topic | 16 个唯一 topic | 16 | PASS |
| 合同遥测 topic | 与寄存器表完全相同 | 16 | PASS |
| 教程必需章节 | 原理、角色、代码、边界、资料齐全 | 6 项检查通过 | PASS |
| 三份冻结基线引用 | 文件存在 | 3/3 | PASS |

检查使用 PowerShell 内存脚本完成，没有生成持久临时脚本或日志副本。

## 4. 人工审核结果

用户已于 2026-08-09 审核通过以下决策：

1. MQTT 3.1.1、QoS 1、Clean Session 和应用有界积压是否保持；
2. 遥测不 MQTT retain、设备/网关状态 retain 是否符合预期；
3. `source_timestamp` 作为 RTU 响应接收时间代理是否可接受；
4. Client ID 的 23 字节兼容边界、20 秒 Keep Alive 和 5 秒连接超时是否可接受；
5. LWT 时间戳表示注册时间而非断线时间是否说明充分；
6. `raw_value=null` 表示尚无任何样本的质量消息是否可接受；
7. 初学者教程是否达到所需深度。

## 5. 明确未执行

- 未安装 Eclipse Paho、Mosquitto 或其他 MQTT 工具；
- 未修改独立仓库代码、CMake、配置或测试；
- 未运行 broker、MQTT publish/subscribe 或断线恢复；
- 未验证 TLS、认证、ACL、公网连接或生产部署；
- 未执行任何 Git 操作或 Git 工作区检查。

## 6. 后续阶段门

T01 固化完成后，下一步是单独制定 `P3-S4-T02` 测试先行实施方案。未完成本地 broker 的
发布、断线、重连和有界关闭测试前，G3 MQTT 门保持未通过。
