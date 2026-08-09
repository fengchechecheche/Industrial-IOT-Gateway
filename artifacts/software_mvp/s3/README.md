# P3-S3-T05 开发验收证据索引

本目录记录 2026-08-09 在 `Ubuntu-24.04-Gateway` 上完成的 P3-S3-T05 开发态证据。

- `quality_gate_summary.json`：机器可读的构建、测试和范围摘要；
- `docs/p3_s3_t05_validation.md`：完整中文验收记录；
- 正常、故障、压力、断线重连和 SIGTERM 用例由 CTest 中的
  `gateway_runtime_pty_test` 与 `gateway_app_process_test` 提供。

当前证据不绑定提交哈希，因此不能替代 `artifacts/baseline/<run_id>/` 下的提交后正式基线。
本任务未执行 Git 暂存、提交或推送，也未执行 MQTT、systemd、长稳或真实 RS485 验收。
