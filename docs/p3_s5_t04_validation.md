# P3-S5-T04 预跑与容量评估报告

> 验收日期：2026-08-13  
> 工作块：30–60 分钟预跑与容量评估  
> 结论：**FAIL；S6 软件 8 小时长稳测试暂时阻塞**

## 1. 验收结论

本次使用冻结的 `software_preflight_v1` profile 连续执行了约 3600 秒，运行过程完整结束，
runner、driver、Mosquitto、MQTT subscriber 和 PTY 仿真链均完成有界清理。资源、吞吐、时延、
队列、七类故障触发与恢复、日志轮转及 SHA-256 完整性检查均通过。

但机器摘要发现一个 P1：在 650 ms 延迟响应故障期间，非目标从站的最大成功间隔为 13.5 秒，
超过冻结的 10 秒公平性门限。除此之外，按预跑末尾实测写入速率线性外推，8 小时证据量约为
1.142 GiB，预计约 7.0 小时触碰单次证据 1 GiB 上限。因此本次不能判为 PASS，也不能直接
启动 S6。

`manifest.status=PASS` 只表示 runner 按计划运行并收尾；最终验收必须以
`summary.status=FAIL` 和 `failures.json` 为准。`long_soak_pass=false` 符合 preflight 结论边界。

## 2. 固定输入与正式证据

| 字段 | 值 |
|---|---|
| source revision | `e38390ee560fa0573b3aebf16ccc6fc225361400` |
| profile | `software_preflight_v1` / `preflight` |
| 开始时间（UTC） | `2026-08-12T15:39:17.151Z` |
| 结束时间（UTC） | `2026-08-12T16:39:17.776Z` |
| monotonic duration | 3600.625 s |
| run ID | `20260812T153917Z_soak_preflight_e38390e_001` |
| 正式证据目录 | `artifacts/soak/preflight/20260812T153917Z_soak_preflight_e38390e_001/` |
| worktree 记录 | `not_checked_by_policy` |
| summary | FAIL，未关闭失败 1 |
| SHA-256 | `sha256sum --quiet -c SHA256SUMS` PASS |

本次没有检查 Git 工作区状态，没有执行 `git add`、`git commit` 或 `git push`。

## 3. 关键结果

### 3.1 性能与资源

| 指标 | 实测 | 冻结门限 | 结果 |
|---|---:|---:|---|
| 正常请求成功率 | 100% | ≥ 99.9% | PASS |
| 正常吞吐 | 26.201 requests/s | ≥ 20.96 requests/s | PASS |
| 正常 P95 / P99 / 最大时延 | 0 / 0 / 6 ms | ≤ 100 / ≤ 250 / < 500 ms | PASS |
| 最大 heartbeat 间隔 | 5.907 s | ≤ 30 s | PASS |
| RSS 峰值 | 9.941 MiB | ≤ 256 MiB | PASS |
| RSS 斜率 | 0.444 MiB/hour | ≤ 2 MiB/hour | PASS |
| 稳态 RSS 差 | 0.414 MiB | ≤ 16 MiB | PASS |
| CPU 平均 / P95 | 2.511% / 2.389% | ≤ 50% / ≤ 80% | PASS |
| fd 峰值 / 稳态漂移 | 12 / 0 | ≤ 64 / ≤ 4 | PASS |
| 线程峰值 / 稳态漂移 | 11 / 0 | ≤ 32 / ≤ 2 | PASS |
| 关闭耗时 | 8 ms | ≤ 5000 ms | PASS |

七类故障均触发、产生预期分类并恢复。正常窗口共记录 80095 个请求事件，MQTT 记录 92870 条
消息；request、measurement 和 publish 队列最大深度分别为 1/256、12/512 和 473/1024，
未出现 request rejection、measurement enqueue failure、critical enqueue failure、
`drain_expired` 或无法解释的数据丢弃。

### 3.2 P1：非目标从站公平性失败

`failures.json` 的唯一失败为：

| failure ID | oracle | 期望 | 实测 | 严重度 |
|---|---|---:|---:|---|
| `SOAK-001` | `requests.non_target_fairness` | ≤ 10.0 s | 13.5 s | P1 / OPEN |

定向复核显示，失败发生在约 1200–1215 秒的 slave 3 延迟故障窗口：slave 1 最大成功间隔为
13.5 秒，slave 2 为 9.461 秒。目标从站的 20 次 `response_timeout` 和后续恢复均被正确识别，
但这不能抵消非目标从站公平性失败。

基于证据与当前源码，可作出以下根因推断：`response_timeout` 为 500 ms，而故障响应延迟为
650 ms。`execute_request()` 只会在发送下一请求前调用 `discard_late_bytes()` 清空当时已经到达
的字节；约 150 ms 后才到达的晚帧不会被这次清空捕获。晚帧随后进入其他请求的解析上下文，
引发连续的超时或协议不匹配，扩大了非目标从站的成功间隔。T03 的修复避免了错误关闭健康
PTY，但尚未建立“超时后等待并丢弃晚帧直至总线重新同步”的状态。

### 3.3 日志轮转与容量外推

- 最后一个资源样本：3596.099 秒时证据为 153,110,205 bytes；
- 实测平均增长约 42,576.75 bytes/s；
- 线性外推 8 小时约 1,226,210,464 bytes，即 1.142 GiB；
- 按该速率约 7.005 小时触碰 1 GiB 单次证据上限；
- 收尾后的实际目录为 153,325,844 bytes；
- 共 10 个分段文件，最大段为 67,107,966 bytes，小于 64 MiB 的 67,108,864-byte 上限；
- 运行末尾磁盘剩余约 951.7 GiB，不存在宿主磁盘空间不足。

因此“日志能够轮转”已经通过，但“按当前证据策略在 1 GiB 内完成 8 小时运行”尚不满足。
8 小时外推是容量预测，不替代真实 release 观测；它已经足以在启动 S6 前暴露预算冲突。

## 4. S6 前必须关闭的事项

1. 为超时路径设计有界的晚帧隔离/排空状态：隔离期间不得发送新请求；收到字节后至少等到
   RTU 3.5 字符静默再恢复，同时设置最大隔离预算，防止永久阻塞。
2. 增加 650 ms 延迟响应的 PTY 集成回归，明确断言 slave 1/2 的最大成功间隔均不超过 10 秒，
   并验证健康串口不被关闭、晚帧不进入后续请求上下文。
3. 对证据容量作版本化决策：优先评估对高重复原始消息进行无损压缩或等价的有界降量；若要
   调整 1 GiB 上限，必须记录磁盘预算与原因，形成新 profile 版本，不能改写本次 FAIL。
4. 修复后通过定向测试与 Debug、ASan/UBSan、Release、clang-tidy、TSan、clang-format、CTest，
   由用户确认新提交 ID，再完整重跑 3600 秒 preflight。
5. 只有新预跑的 `summary.status=PASS` 且 8 小时容量预算成立，才解除 S6 阻塞。

## 5. 结论边界

本次证明：预跑工具可连续运行一小时；资源占用稳定；七类故障、MQTT 重连、PTY 重连、结构化
证据、日志轮转和有界关闭能够工作。本次没有证明：冻结公平性门限已满足、1 GiB 证据预算足以
覆盖 8 小时、8 小时软件长稳已通过、树莓派部署已通过或真实 RS485/STM32 硬件稳定。

正式失败证据必须保留，不能因为后续修复而删除或改写。
