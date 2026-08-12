# P3-S5-T04 阻塞项修复验收报告

> 验收日期：2026-08-13  
> 范围：晚响应跨事务污染修复、10 秒公平性门限保留、5 GiB v2 证据预算  
> 当前结论：**实现与质量门 PASS；绑定新提交的 3600 秒 preflight 待执行**

## 1. 结论

首次 T04 预跑发现的 P1 `SOAK-001` 已完成根因验证和最小修复。代码现在会在
`response_timeout` 或 `truncated_frame` 后进入 200 ms 有界隔离期，不发送下一请求；隔离期
内到达的字节被丢弃，收到字节后等待 19200 8E1 的 RTU 3.5 字符静默边界再恢复调度。最长
隔离还受最大 RTU ADU 传输预算约束，不会因坏从站永久阻塞总线。

公平性门限继续保持 10 秒，没有放宽到 20 秒。证据预算通过新建 v2 profile 调整为 5 GiB，
同时把启动和运行时剩余磁盘门限提高为 10 GiB、5 GiB；v1 profile 和首次 FAIL 证据均保留。

Debug、ASan/UBSan、Release 和 TSan 均完成 182/182 CTest；clang-tidy 全目标构建完成，本次
新增代码没有新增诊断；全项目 clang-format、Python、JSON 和 v2 validate-only 通过。

当前不能把本报告写成 T04 最终 PASS：正式 preflight 必须绑定用户提交后的新 SHA，完整运行
3600 秒并得到 `summary.status=PASS`。在此之前 S6 仍被阻塞。

## 2. 旧失败根因验证

旧正式证据：

`artifacts/soak/preflight/20260812T153917Z_soak_preflight_e38390e_001/`

绑定提交：`e38390ee560fa0573b3aebf16ccc6fc225361400`。

在延迟窗口中，旧证据形成了可复核的时间序列：

| 相对时间 | 从站 | 结果 | 持续时间 | 证据位置 |
|---:|---:|---|---:|---|
| 1200.503 s | 3 | `response_timeout` | 500 ms | `gateway_0001.jsonl:93735` |
| 1200.658 s | 1 | `serial_io_transient` | 155 ms | `gateway_0001.jsonl:93737` |
| 1201.204 s | 3 | `response_timeout` | 500 ms | `gateway_0001.jsonl:93750` |
| 1201.359 s | 2 | `serial_io_transient` | 155 ms | `gateway_0001.jsonl:93754` |

650 ms 延迟比 500 ms timeout 晚约 150 ms。后续非目标请求在约 155 ms 后出现
`serial_io_transient`，与晚帧进入下一接收上下文一致；随后 slave 1/2 和目标从站出现连续
timeout/协议错配。slave 1 在窗口内最后一次成功为 1201.500 s，直到窗口结束都没有再次成功，
最大成功间隔为 13.5 秒；slave 2 为 9.461 秒。

源码审计进一步确认旧机制只能在下一次发送前调用 `discard_late_bytes()`，清空调用当时已经在
接收缓冲区中的数据。它无法等待并捕获未来约 150 ms 后才到达的旧响应。因此“晚帧跨事务污染”
不是仅凭最终 13.5 秒结果推测，而是由时间序列与旧代码机制共同支持的根因结论。

## 3. 修复实现

### 3.1 有界隔离状态

- `GatewayRuntimeConfig::late_response_guard` 默认 200 ms；0 ms 或超过 1 秒的配置被拒绝；
- timeout/截断后调用 `quarantine_late_response()`；
- 无字节时最多监听 200 ms，覆盖本场景约 150 ms 的晚到量；
- 收到字节后继续排空，直到 2.006 ms RTU 帧间静默；
- 最长附加预算包含最大 256-byte RTU ADU 在 19200 8E1 下约 150 ms 的传输时间；
- 隔离期间串行线程不发送下一请求；
- 只有真实 wait/read/peer I/O 故障才要求关闭串口。

### 3.2 可观测性

新增：

- `late_response_quarantines`；
- `late_response_bytes_discarded`；
- `late_response_quarantine_started`；
- `late_response_discarded`；
- `late_response_quarantine_completed`。

soak driver 把两个累计计数写入 heartbeat 和最终统计；summary 检查它们单调不减，并把最终值
写入 metrics。每个 `delayed_response` 故障循环还必须出现至少一个
`late_response_discarded`，否则机器摘要直接 FAIL。

## 4. 测试先行与修复验证

测试先行阶段先增加了以下尚不存在的合同，旧实现按预期失败：

- v2 release/preflight 文件缺失，profile 单测 4 项 ERROR；
- `GatewayRuntimeConfig` 缺少 `late_response_guard`，C++ 红灯构建失败；
- 运行时缺少隔离计数和结构化事件。

实现后：

| 验证 | 结果 |
|---|---|
| `RejectsZeroLateResponseGuardBeforeStartingThreads` | PASS |
| `QuarantinesLateResponseWithoutPollutingNextTransaction` | PASS，约 1.9 s |
| 隔离至少丢弃一个完整晚响应 | PASS，`late_response_bytes_discarded` 至少增加 5 |
| 健康串口不重开 | PASS，`serial_open_successes` 保持 1 |
| 后续请求不新增串行错误 | PASS |
| slave 1/2/3 故障清除后继续成功 | PASS |
| 缺少 `late_response_discarded` 的摘要反例 | PASS：机器摘要按预期 FAIL |
| 90 秒完整 soak smoke | PASS，含七类故障和新增强制 oracle |

这条验收链分别证明了“旧机制为何失败”“修复路径确实捕获晚帧”和“后续自动化会持续检查该
根因”，不是仅通过最终 smoke PASS 倒推根因已经解决。

## 5. v2 门限决策

### 5.1 公平性保持 10 秒

故障持续窗口为 15 秒。若门限放宽到 20 秒，即使健康从站在整个故障窗口零成功，最大间隔也
只有 15 秒，oracle 仍会 PASS，因此 20 秒会使当前判定失去意义。v1/v2 均继续使用：

```json
"non_target_starvation_seconds_max": 10
```

### 5.2 证据预算调整到 5 GiB

首次预跑外推 8 小时约 1.142 GiB，说明 1 GiB 是容量预算偏紧，不是运行时可靠性失败。v2 使用：

```json
{
  "start_free_gib_min": 10,
  "runtime_free_gib_min": 5,
  "evidence_bytes_max": 5368709120,
  "log_segment_bytes_max": 67108864
}
```

单段仍限制为 64 MiB。新建 `software_release_v2` 和继承它的 `software_preflight_v2`，没有覆盖
v1，也没有用新门限改写首次 FAIL。

## 6. 质量门

| 质量门 | 结果 |
|---|---|
| Debug + MQTT + PTY + warnings-as-errors | PASS，182/182 |
| ASan/UBSan + MQTT + PTY | PASS，182/182 |
| Release + MQTT + PTY | PASS，182/182 |
| TSan + WSL2 workaround + MQTT + PTY | PASS，182/182 |
| clang-tidy 18 | 全目标构建完成；仅既有 optional/异常逃逸提示 |
| clang-format 18 | PASS，全项目 `--dry-run --Werror` |
| Python profile/summary 定向单测 | PASS，10/10 |
| Python `py_compile` 与 JSON parse | PASS |
| `software_preflight_v2` validate-only | PASS，`PROFILE_VALID=software_preflight_v2` |

## 7. 待完成验收

1. 用户提交当前实现并提供新提交 ID；
2. 以 Release driver、`software_preflight_v2.json` 和新 SHA 执行 3600 秒正式 preflight；
3. 公平性仍必须 ≤10 秒；
4. 每个 delayed 故障循环必须包含 timeout 和 `late_response_discarded`；
5. 证据、资源、队列、MQTT、七类故障和 SHA-256 全部门继续 PASS；
6. 只有 `summary.status=PASS`、`unclosed_failures=0` 后，才更新最终 T04 状态并解除 S6 阻塞。

本阶段没有检查 Git 工作区状态，也没有执行 `git add`、`git commit` 或 `git push`。
