# 软件长稳测试协议

> 合同版本：1.0.0  
> 适用工作块：P3-S5-T03 及其后续 T04/S6 软件长稳执行  
> 当前状态：合同与工具已实现；3600 秒预跑和 28800 秒正式长稳尚未执行

## 1. 目的与结论边界

本协议冻结 PTY + 本地 Mosquitto 软件长稳测试的负载、故障、门限、证据和退出码。T03 的
90 秒 smoke 只证明工具链、故障调度和判定链能够运行，不能声明 1 小时预跑或 8 小时稳定性
已经通过，也不能替代真实 RS485、STM32 或传感器台架证据。

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
