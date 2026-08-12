# 软件故障矩阵运行器

本目录实现可重复的软件故障注入编排。`P3-S4-T03` 建立初始 G3 基线，`P3-S5-T01`
使用同一份已审核 `software` profile 进行发布前复核。运行器使用 Python 标准库逐项调用
CTest 场景，保证超时后回收整个进程组，并把机器可判定的结果写入结构化证据目录。

## 运行前提

- 在 WSL 的 Linux 文件系统中完成包含 PTY 与 MQTT 的测试构建；
- 本机已安装项目固定版本的 Paho MQTT、Mosquitto 与 nlohmann/json；
- 使用回环地址上的匿名 broker；
- 不需要 root 权限，也不读取 Git 工作区状态。

## 探索性运行

源码尚未形成可追溯提交时，只能生成探索性证据：

```bash
python3 tools/run_fault_matrix.py \
  --profile software \
  --build-dir /tmp/industrial_iot_gateway_s5_t01_debug \
  --output-root /tmp/industrial_iot_gateway_s5_t01_exploratory \
  --source-revision uncommitted \
  --exploratory \
  --stage S5 \
  --task P3-S5-T01
```

探索性结果可用于调试和候选验收，但不能冒充绑定到提交的正式 G3 基线。

## 正式运行

正式运行要求调用者显式提供 7–40 位十六进制源码版本：

```bash
python3 tools/run_fault_matrix.py \
  --profile software \
  --build-dir /tmp/industrial_iot_gateway_s5_t01_debug \
  --output-root artifacts/fault_matrix/s5 \
  --source-revision 0123456789abcdef \
  --stage S5 \
  --task P3-S5-T01
```

运行器只校验参数格式，不自行调用 Git。执行完成后，退出码 `0` 表示十五个场景均通过；
参数或 profile 错误返回 `2`，构建目录不存在返回 `3`，场景失败返回 `4`，证据完整性
错误返回 `5`。

`--stage` 和 `--task` 会原样写入 `manifest.json`。默认值仍为 `S4` 和 `P3-S4-T03`，以兼容
既有基线命令；执行 S5 时必须显式传入上述两个参数，防止证据所属阶段被错误标注。

## 证据结构

每次运行创建唯一的 `<run_id>/` 目录。根目录保存 `manifest.json`、`summary.json`、
`failures.json`、`events.jsonl`、环境、命令、日志和 `SHA256SUMS`；
`scenarios/F01` 至 `scenarios/F15` 各自保存：

- `config.yaml`：场景参数与预期结果；
- `events.jsonl`：带 `run_id`、`scenario_id` 和唯一 `event_id` 的事件；
- `summary.json`：实际状态、判定器、进程结果、恢复时间和证据事件 ID；
- `failures.json`：未通过判定器的结构化失败记录。

F06、F10、F11 和 F13 必须由集成测试输出
`FAULT_RECOVERY_TIME_MS=<非负整数>`。缺少该指标时，即使 CTest 返回零，场景也会失败。
其他场景不存在明确的“撤销故障后恢复”阶段，其 `recovery_time_ms` 保持 `null`，避免把
整个测试进程耗时误写成恢复时间。

## 安全边界

- profile 命令以参数数组执行，不经过 shell 拼接；
- 每个场景有独立超时，超时先向进程组发送 SIGTERM，宽限期后再发送 SIGKILL；
- 一个场景失败后仍继续执行其余场景，最终统一返回失败；
- `SHA256SUMS` 覆盖本次运行的全部证据文件；
- `artifacts/baseline/` 与 `artifacts/fault_matrix/` 默认作为本地证据处理，是否归档或公开必须
  由项目所有者另行决定。
