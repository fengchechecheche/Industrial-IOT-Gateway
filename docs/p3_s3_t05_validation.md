# P3-S3-T05 PTY 软件集成矩阵验收记录

## 1. 验收结论

- 任务：`P3-S3-T05`（PTY 软件集成矩阵）。
- 执行日期：2026-08-09。
- 环境：`Ubuntu-24.04-Gateway`，x86_64，WSL2，仓库位于 Linux 文件系统。
- WSL 开发验收：**通过**。
- Debug、ASan/UBSan、Release、clang-tidy、TSan：**全部通过，150/150**。
- 提交后固定版本 Release 复验：**未执行**；本次没有获得 Git 暂存、提交或推送授权。

本任务把 S2 的 Modbus RTU codec/parser、PTY 从站，以及 S3 T01–T04 的串口、调度、可靠性、
新鲜度、有界队列、关闭和结构化日志连接为可运行的软件网关主循环。`gateway_app` 现在可以加载
冻结的 `register_map.yaml`，通过指定串口设备轮询 3 个从站的 16 个点位，并在 SIGINT/SIGTERM
后有序停止。

该结论只证明 G2 范围内的 Linux/PTY 软件集成，不表示 MQTT、本地 broker、8 小时长稳、systemd、
UART/USB-RS485、RS485 电气层或真实硬件已经完成。整个软件 MVP 仍需后续 G3–G5 任务共同通过。

## 2. 运行时结构

运行时采用以下所有权边界：

1. scheduler 线程唯一持有 `PollScheduler`，产生周期请求并消费尝试结果；
2. serial I/O 线程唯一持有 `SerialPort` 文件描述符，执行编码、有限写入、有限等待和响应解析；
3. quality 线程负责类型解码、工程量换算、fresh/stale/offline 转换；
4. evidence sink 线程把遥测、质量转换和写审计输出为线程安全 JSONL；
5. 请求、测量、发布、scheduler feedback 和 quality control 均使用固定容量队列；
6. 停止请求依次关闭入口、唤醒等待、结束串口所有者并 join 全部线程。

单总线仍由 serial I/O 线程串行执行，运行统计验证 `maximum_in_flight_requests == 1`。MQTT 未接入，
发布队列当前由 JSONL evidence sink 消费，避免提前把本任务宣称为 MQTT 集成。

## 3. 配置与数据语义

`RuntimeConfiguration` 从冻结的 `config/examples/register_map.yaml` 读取：

- schema 版本必须为 `1.0.0`；
- 只加载启用设备和启用轮询项；
- 冻结为 3 个从站、16 个轮询作业，作业 ID 按 YAML 顺序稳定生成；
- 校验 slave、功能码、地址、寄存器数量、数据类型、周期、新鲜度、scale 和 topic；
- 支持 `uint16`、`int16`、`uint32`、`int32` 和 high-word-first IEEE-754 `float32`；
- 工程量换算使用 `decoded * scale + offset`，非有限浮点数和寄存器数量不匹配显式失败。

缺失文件、YAML 语法错误和非法字段均在创建线程前失败。

## 4. 四路 PTY 测试拓扑

测试夹具创建一个网关侧 PTY 和三个相互独立的从站 PTY。测试专用总线中继按 RTU 地址把网关
请求路由到对应从站，并只在测试代码中存在；生产运行时只看到一个串口设备，不依赖测试夹具。

稳定符号链接用于断线重连测试：夹具在旧 PTY 仍占用时创建新 PTY、原子更新别名、关闭旧主端，
并在恢复中继前丢弃断线期间积压的过期请求。夹具析构时关闭所有描述符、join 所有线程并删除
临时符号链接。

## 5. 集成矩阵

| 场景 | 判定结果 | 关键断言 |
|---|---|---|
| 三从站正常轮询 | PASS | 3 个 slave 均成功，16 点位产生遥测，最多 1 个在途请求 |
| 显式 0x06 写请求 | PASS | 写回显成功，写审计可观察 |
| 慢响应 | PASS | 60 ms 延迟仍在有限 deadline 内成功，其他从站继续轮询 |
| 静默从站 | PASS | 分类为 `response_timeout`，不会永久阻塞其他从站 |
| 坏 CRC | PASS | 分类为 `crc_mismatch` |
| 截断响应 | PASS | 分类为 `truncated_frame` |
| Modbus 异常响应 | PASS | 分类为 `remote_exception`，保留异常码 |
| 队列压力 | PASS | 2000 个显式写产生可观察 `full`，容量不增长，关闭无死锁 |
| 断线与重连 | PASS | 稳定设备路径重新打开，成功计数继续增长 |
| stale/offline/恢复 | PASS | JSONL 出现 stale、offline，链路恢复后轮询成功计数继续增长 |
| SIGTERM | PASS | 真实子进程收到 SIGTERM，线程 join、串口关闭、摘要 `stopped=true` |

实现过程中发现并修复两个只有整链路测试才能暴露的问题：

- 写失败此前没有先把尝试推进到 `sent`，调度器拒绝结果并把作业留在 `dispatched`；现在每次 I/O
  尝试开始前先发送状态反馈，真正写到线上的统计仍单独计算；
- PTY 断开后，非阻塞写可能持续返回 would-block/ready；写循环现在在每次迭代检查
  `steady_clock` deadline，并显式处理 peer-closed，保证有限退出和后续重开。

## 6. gateway_app

运行入口：

```bash
gateway_app --serial-device PATH --register-map config/examples/register_map.yaml
```

可选的本地测试写入口为：

```bash
--write-slave ID --write-address ADDRESS --write-value VALUE
```

三个写参数必须同时出现。应用在创建工作线程前阻塞 SIGINT/SIGTERM，主线程使用 `sigwait()` 同步
等待；就绪时输出 `gateway_ready`，关闭后输出包含成功/失败计数、串口打开次数和 `stopped` 的
`gateway_summary` JSONL。

## 7. 测试先行与回归

测试先行阶段依次得到以下预期红灯：

1. 配置加载、类型解码和阻塞队列等待 API 未定义导致链接失败；
2. `GatewayRuntime` 公共接口存在但实现未接入导致链接失败；
3. 四路 PTY 首轮集成暴露断线后调度状态停滞；
4. stale/offline 恢复测试暴露夹具积压旧请求造成响应错配。

加入最小实现并修复整链路问题后：

- `gateway_runtime_unit_test`：11/11；
- `gateway_runtime_pty_test`：9/9；
- `gateway_app_process_test`：1/1；
- 全量 CTest：150/150。

## 8. 质量门

所有构建均在 `/tmp/industrial_iot_gateway_t05_*` 中执行，避免把临时构建产物带入提交边界。

| 配置 | 构建 | CTest | 补充结果 |
|---|---|---:|---|
| Debug | 通过 | 150/150 | 警告视为错误，PTY 开启 |
| Debug + ASan/UBSan | 通过 | 150/150 | 无 sanitizer 报告 |
| Release | 通过 | 150/150 | PTY 开启 |
| Debug + clang-tidy | 通过 | 150/150 | clean-first，最终零告警 |
| Debug + TSan | 通过 | 150/150 | WSL2 进程级 `setarch` 入口，无数据竞争报告 |

最终标签统计为 `unit` 126、`integration` 20、`pty` 4。16 个本任务 C++ 文件通过
`clang-format-18 --dry-run --Werror`。clang-tidy 首轮的 10 条告警均已修复，最终 clean-first
构建不再输出告警。

## 9. 证据与提交边界

开发态结构化摘要位于 `artifacts/software_mvp/s3/quality_gate_summary.json`，证据索引位于同目录
`README.md`。这些文件记录当前工作区开发验收，不冒充提交后固定版本证据。

正式基线仍应在用户完成提交后，以提交哈希作为 `run_id` 重新运行相同门禁，并把不可变日志保存到
`artifacts/baseline/<run_id>/`。本任务未运行 `git status`、工作区差异检查、`git add`、
`git commit` 或 `git push`。

## 10. 后续边界

T05 向 S4 移交稳定的运行时接口、JSONL 遥测/质量对象、故障分类和 PTY 软件集成基线。下一步可
开始 MQTT 发布与本地 broker 集成，但不得把当前 JSONL evidence sink 描述为 MQTT 发布，也不得
把 PTY 重连描述为真实 RS485 电气验证。
