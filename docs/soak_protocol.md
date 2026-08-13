# 软件长稳测试协议

> 合同版本：1.0.0  
> 适用工作块：P3-S5-T03 及其后续 T04/S6 软件长稳执行  
> 当前状态：v1 首次预跑 FAIL 已保留；v2 3600 秒预跑与 28800 秒正式长稳均 PASS

## 1. 目的与结论边界

本协议冻结 PTY + 本地 Mosquitto 软件长稳测试的负载、故障、门限、证据和退出码。T03 的
90 秒 smoke 只证明工具链、故障调度和判定链能够运行。后续一小时 preflight 已完整执行，
但因非目标从站公平性和 8 小时证据预算问题未通过，不能声明 1 小时预跑或 8 小时稳定性已经
通过，也不能替代真实 RS485、STM32 或传感器台架证据。

## 2. 冻结 profile

| profile | 文件 | 时长 | warm-up | 故障循环 | 可形成长稳 PASS |
|---|---|---:|---:|---:|---|
| smoke | `tests/data/soak_profiles/software_smoke.json` | 90 s | 5 s | 1 | 否 |
| preflight | `config/soak/software_preflight.json` | 3600 s | 300 s | 1 | 否 |
| release | `config/soak/software_release.json` | 28800 s | 600 s | 8 | 是 |

preflight 继承 release，只覆盖时长、warm-up、循环次数和 profile 标识。两者的负载、阈值、
故障类型与证据语义必须完全一致。smoke 缩短日程但保持相同的七类故障和判定语义。

正式 profile 必须提供 7～40 位十六进制 `source_revision`，并把原始证据写入
`artifacts/soak/`；`exploratory` 只允许 smoke。validator 会拒绝远程 broker、越出仓库的配置
路径、重叠故障、重复 fault ID 和缺失门限。

## 3. 冻结负载

- PTY 单总线，19200 baud、8E1；
- 三个 Modbus RTU 从站：环境传感器、执行器、故障注入设备；
- 直接使用 `config/examples/register_map.yaml` 中的 16 个轮询作业；
- 本地匿名 Mosquitto，MQTT 3.1.1、QoS 1，订阅 `industrial_iot_gateway/#`；
- runner、gateway soak driver、broker 和 subscriber 均为独立进程；
- 资源采样来自 Linux `/proc` 和文件系统统计，不依赖第三方 Python 包。

## 4. release/preflight 故障日程

每 3600 秒一个循环，目标协议从站固定为 slave 3。七个窗口不重叠；每一个循环都必须分别
出现触发、预期错误类别和恢复证据，不能用某一循环的成功代替其他循环。

| 偏移 | 故障 | 持续 | 预期分类 | 恢复门限 |
|---:|---|---:|---|---:|
| 600 s | slave 3 静默 | 15 s | `response_timeout` | 15 s |
| 1200 s | 响应延迟 650 ms | 15 s | `response_timeout` | 15 s |
| 1800 s | 坏 CRC | 15 s | `crc_mismatch` | 15 s |
| 2400 s | 截断帧 | 15 s | `truncated_frame` | 15 s |
| 2700 s | 异常码 0x02 | 15 s | `remote_exception` | 15 s |
| 3000 s | broker 停止 | 30 s | MQTT disconnect/reconnect | 45 s |
| 3300 s | PTY 总线断开 | 3 s | `serial_io_transient` | 15 s |

## 5. PASS 门限

### 5.1 生命周期、数据和恢复

- release monotonic 时长不少于 28800 秒；
- driver 正常启动、到时停止，`stopped=true`，关闭不超过 5000 ms；
- heartbeat 最大间隔不超过 30 秒；
- 意外退出、critical 事件、JSON 解析错误和未分类请求错误均为 0；
- 每循环七类故障均触发、产生预期分类并恢复；
- stale/offline 历史值必须带 `value_is_retained=true`，invalid 不得带工程量；
- 正常窗口成功率不少于 99.9%，吞吐不少于 20.96 requests/s；
- 正常成功请求 P95 不超过 100 ms、P99 不超过 250 ms、最大值小于 500 ms；
- 故障期间 slave 1/2 任一成功间隔不超过 10 秒。

### 5.2 队列、MQTT 和资源

- request、measurement、publish 队列深度不得超过固定容量；
- request rejection、measurement enqueue failure、critical enqueue failure、drain expired 和
  unconfirmed on close 均为 0；
- MQTT `dropped == expired_fresh_dropped`，普通 fresh 合并允许非零但必须计数；
- 所有累计统计在 heartbeat 与最终快照之间单调不减；
- RSS 峰值不超过 256 MiB；warm-up 后先形成 5 分钟 RSS 中位数桶，再计算斜率，斜率不超过
  2 MiB/hour；首末稳态 30 分钟 RSS 中位数差不超过 16 MiB；
- 单核 CPU 平均不超过 50%，P95 不超过 80%，不得连续 300 秒高于 90%；
- fd 峰值不超过 64、稳态漂移不超过 4；线程峰值不超过 32、稳态漂移不超过 2；
- 启动磁盘不少于 5 GiB、运行时不少于 1 GiB、单次证据不超过 1 GiB；
- driver、gateway、PTY、MQTT、broker 和 subscriber 日志均按序号轮转，单段不超过 64 MiB。

## 6. 执行与证据

T04 预跑的目标命令为：

```bash
python3 tools/run_soak.py \
  --profile config/soak/software_preflight.json \
  --source-revision <CONFIRMED_COMMIT_ID> \
  --output-root artifacts/soak/preflight
```

S6 正式运行把 profile 换为 `software_release.json`、目录换为 `artifacts/soak/release`。runner
自动生成 `manifest.json`、解析后的 `profile.json`、环境、命令、事件、资源样本、分段日志、
`summary.json`、`failures.json` 和 `SHA256SUMS`。摘要可重复执行：

```bash
python3 tools/summarize_soak.py artifacts/soak/<kind>/<run_id>
```

只有 `summary.status=PASS` 且 `profile_kind=release` 才会产生
`long_soak_pass=true`。smoke 和 preflight 即使 PASS，该字段也必须为 false。任一受强制 oracle
失败时 runner/摘要命令返回非零；外部环境中止记为 `ABORTED_ENVIRONMENT`，不得改写成 PASS。

## 7. 安全与提交边界

- 原始证据位于已忽略的 `artifacts/soak/`，不得混入公开文档；
- 日志不保存生产凭据，broker 仅允许回环地址；
- 工具不调用 Git，只接收用户确认的提交 ID；
- T03 不自动启动 T04 或 S6；
- 未经项目所有者授权，不执行 `git add`、`git commit` 或 `git push`。

## 8. T04 首次预跑结果

绑定提交 `e38390ee560fa0573b3aebf16ccc6fc225361400` 的首次 3600 秒 preflight 已于
2026-08-13 完成。正式 run ID 为 `20260812T153917Z_soak_preflight_e38390e_001`；runner 完整
执行，SHA-256 复核通过，但 `summary.status=FAIL`。

唯一 P1 为 `requests.non_target_fairness`：实测最大成功间隔 13.5 秒，超过 10 秒门限。日志单段
轮转通过，但按 42,576.75 bytes/s 外推，8 小时证据约 1.142 GiB，超过 1 GiB 总量预算。旧结果
不得通过放宽门限改写；关闭两个阻塞项、形成新提交和新 profile 版本后，必须完整重跑 preflight。

## 9. T04 阻塞项修复后的 v2 候选合同

首次 FAIL 保持不变。后续正式复跑使用新建的 `software_release_v2.json` 和
`software_preflight_v2.json`：

- 非目标从站公平性仍为 10 秒，不放宽到 20 秒；
- timeout/截断后执行 200 ms 有界晚响应隔离；
- 隔离期间不得发送下一请求，收到字节后等待 2.006 ms RTU 静默；
- 每个 delayed 故障循环必须出现 `late_response_discarded`；
- 证据总量上限为 5 GiB；
- 启动/运行时剩余磁盘门限为 10/5 GiB；
- 单日志段仍为 64 MiB。

候选实现先通过四套 182/182 CTest、clang-tidy 全目标和格式检查，随后绑定提交
`ecb4cacdf56e187940b341d647ff1529ca2f0b0c` 完整执行正式复验。

## 10. v2 正式 preflight 结果

run ID `20260812T174640Z_soak_preflight_ecb4cac_001` 完整运行 3600.035 秒，
`summary.status=PASS`、未关闭失败 0。非目标从站最大成功间隔 2.991 秒；晚响应字节丢弃 95、
隔离 37 次；正常成功率 100%、吞吐 26.205 requests/s；RSS 峰值 9.645 MiB、斜率
0.222 MiB/hour。证据 152,232,879 bytes，SHA-256 复核通过。v2 因此升为当前正式合同并解除
S6 软件门阻塞。

## 11. S6 正式 release 结果

run ID `20260812T184831Z_soak_release_ecb4cac_001` 使用同一 Release driver 和冻结配置连续运行
28800.055 秒：

- `summary.status=PASS`、`long_soak_pass=true`、未关闭失败 0；
- 八个循环的 56 个故障窗口全部触发并恢复，212/212 oracle PASS；
- 正常成功率 99.9685%、吞吐 26.2005 requests/s，公平性最大间隔 2.998 秒；
- RSS 峰值 9.902 MiB、斜率 0.0404 MiB/hour，fd/线程稳态漂移均为 0；
- 关闭 4 ms，证据 511,325,274 bytes，SHA-256 复核通过。

该结果建立 WSL2 x86_64 软件长稳结论，不建立树莓派、USB-RS485、STM32、传感器或生产
MQTT 网络的硬件/部署结论。
