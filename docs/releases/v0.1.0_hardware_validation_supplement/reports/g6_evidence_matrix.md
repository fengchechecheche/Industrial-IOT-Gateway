# P3-S7-G6：真实硬件验收证据矩阵

> 文档状态：`APPROVED / FINAL`
>
> 当前判定：`T00～T05=PASS`、`G6=PASS`
>
> 编制日期：2026-08-23

## 1. 使用规则

- 每一行只能由本行列出的证据证明，不能用 PTY、项目五或其他平台结果替代；
- 项目五证据只用于避免重复验收其已冻结对象；两只 TAS 和项目三组合必须使用 G6 新证据；
- 自动 PASS 必须同时满足 summary、failures、退出码、SHA-256 和清理边界；
- 人工确认只证明物理动作发生，不替代恢复时间、公平性或成功率 oracle；
- v0.1.0 Release 保持不可变，G6 属于发布后的真实硬件补充验证；
- CAN 不属于项目三 G6 阻塞项，也不新增 CAN claim。

## 2. G6 总矩阵

| ID | 工作块/验收门 | 关键判据 | 权威证据 | 状态 |
|---|---|---|---|---|
| G6-E01 | 项目五复用边界 | 已验收对象不重复；新增组合不冒用旧证据 | T00 复用矩阵；项目五三份冻结报告 | PASS |
| G6-E02 | TAS 资料与身份 | 两只型号明确；官方寄存器和串口合同冻结 | T00；TAS 官方资料 | PASS_WITH_LIMITATION |
| G6-E03 | 供电/参考/终端 | 12 V 低压支路；A/B-only；两端终端；不推测接地 | T00；最终接线；T02～T04 运行 | PASS_WITH_LIMITATION |
| G6-E04 | TAS-A 单机 | 10 分钟读取、CRC、断电恢复、地址 1/19200 8E1 持久化 | T01 | PASS |
| G6-E05 | TAS-B 单机 | 10 分钟读取、地址迁移到 2、19200 8E1、断电持久化 | T01 | PASS |
| G6-E06 | 三从站正常基线 | 地址 1/2/4 无冲突；2/2/17 点覆盖；JSONL/MQTT | T02，候选 `b35a01e…` | PASS |
| G6-E07 | 单 TAS 支路隔离 | TAS-A/TAS-B 整体支路故障；非目标最大间隔 ≤10 s；自动恢复 | T02 正式三目录 | PASS |
| G6-E08 | 故障矩阵 | TAS、STM32 RESET、broker、USB、SIGTERM、reboot 共 17 场景 | T03，run `20260822T214326Z_g6_t03_e873b5e_001` | PASS |
| G6-E09 | systemd/reboot | 开机自启 ≤120 s；观察 ≥600 s；NRestarts=0 | T03 reboot 证据 | PASS |
| G6-E10 | 3600 秒预跑 | 时长、三从站、MQTT、资源、SHA、清理全部通过 | `20260823T032544Z_g6_t04_preflight_c5ddc36_001` | PASS |
| G6-E11 | 28800 秒硬件长稳 | `hardware_long_soak_pass=true`；failures=[]；全部 oracle PASS | `20260823T043408Z_g6_t04_release_c5ddc36_001` | PASS |
| G6-E12 | 长稳成功率/公平性 | 三从站逻辑成功率均 1.000；最大间隔 4.886/4.895/2.997 s | T04 summary | PASS |
| G6-E13 | MQTT/队列/关闭 | publish failure/drop/drain=0；队列有界；shutdown 194 ms | T04 summary | PASS |
| G6-E14 | 资源与 ARM64 主机 | RSS/fd/thread 稳定；62.8℃；无新增 throttled；证据 <10 GiB | T04 resource evidence | PASS |
| G6-E15 | 异常透明度 | TAS-A 两次尝试超时均隔离并重试成功；16 条 stale 可追踪 | T04 原始 evidence 与异常复盘 | PASS_WITH_OBSERVED_TRANSIENTS |
| G6-E16 | T05 claim 固化 | 旧事实保留；新增事实/claim 有证据；CSV/YAML 一致 | T05 正式产物 | PASS |
| G6-E17 | Release 边界 | v0.1.0 不回写；published=false；hardware_validated=false | T06 Release 文档；T05 边界 | PASS |
| G6-E18 | G6 最终标记 | G6-E01～E17 闭合，T05 文档获批并同步 | T05 最终报告 | PASS |

## 3. 工作块结论

| 工作块 | 结果 | 主要事实 |
|---|---|---|
| T00 | PASS_WITH_RECORDED_LIMITATIONS | 差异化准入与项目五复用边界闭合 |
| T01 | PASS | 两只 TAS 单机 1200/1200，地址 1/2 与 19200 8E1 持久化 |
| T02 | PASS | 三从站 JSONL/MQTT 与两个 TAS 对称支路故障 PASS |
| T03 | PASS | 17 个真实故障/systemd 场景及 579 项哈希 PASS |
| T04 | PASS | 3600 秒预跑和 28800 秒真实硬件长稳 PASS |
| T05 | PASS | evidence、claim、CSV 和声明边界已审核并固化 |

## 4. 当前限制

- 没有万用表实测 USB-RS485、Shield 或 TAS A/B 阻值；
- 没有校准设备和参考仪器，温湿度只声明持续更新与合理趋势；
- 只覆盖同桌短线拓扑，不覆盖长距离、EMC、浪涌和隔离；
- CAN 只引用项目五历史结论，不是项目三 G6 结果；
- 单次八小时 PASS 不是工业级、生产可用或无限期可靠性认证；
- 未创建 Git tag 或 GitHub Release。

## 5. 最终关闭条件

本矩阵、claim ledger、CSV 与 T05 验收报告已经审核并同步到独立仓库，正式关闭标记为：

```text
PASS_P3_S7_G6_T05_EVIDENCE_AND_CLAIMS_FINALIZED
PASS_P3_S7_G6_REAL_HARDWARE_SUPPLEMENTAL_VALIDATION
G6=PASS
```

这表示“v0.1.0 发布后的真实硬件补充验证完成”，不改变既有 v0.1.0 文件、manifest 或 Release
index 中的 `hardware_validated=false`。
