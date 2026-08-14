# P3-S7-ARM64-T02：树莓派 ARM64 原生 systemd 十场景验收报告

> 结论：**PASS**  
> 验收标记：`PASS_P3_S7_ARM64_T02_NATIVE_SYSTEMD_TEN_SCENARIOS`  
> 候选提交：`124724e00f6ef2c9d03323198dd618a0af1ae7cd`  
> 正式 run：`20260814T181641Z_systemd_124724e0_001`  
> 执行日期：2026-08-15

## 1. 验收结论

精确候选提交已在 Raspberry Pi 4B 的 Ubuntu Server 24.04 ARM64 原生环境中完成 Release 构建、完整 CTest 和 systemd 十场景验收。runner 真实退出码为 0，十场景 10/10 PASS，`failures.json=[]`，证据哈希全部有效，运行后固定系统对象和测试进程清理完成。

本结论证明当前候选具备 **树莓派 ARM64 软件集成和原生 systemd 服务管理能力**。它不证明真实 RS485/CAN 硬件已验证，也不等同于 ARM64 8 小时长稳或 ARM64 Release bundle 已完成。

## 2. 冻结输入

| 项目 | 值 |
|---|---|
| 候选提交 | `124724e00f6ef2c9d03323198dd618a0af1ae7cd` |
| 目标平台 | `linux-arm64` |
| 实际平台 | `linux-arm64` |
| machine / 用户空间 | `aarch64` / `arm64` |
| 板卡 | Raspberry Pi 4 Model B Rev 1.5 |
| 操作系统 | Ubuntu 24.04.4 LTS |
| 内核 | `6.8.0-1060-raspi` |
| systemd | `255.4-1ubuntu8.17` |
| 虚拟化 | `none` |
| Release 构建 | 原生构建，固定 `--parallel 4` |
| CTest | 191/191 PASS |

关键输入哈希已同时写入正式 `inputs.json` 和去敏矩阵。正式证据所记录的两个二进制、runner、平台分类器、场景 oracle、unit 和两份配置哈希与 CP-F 完全一致。

## 3. 十场景结果

| ID | 场景 | 关键结果 | 状态 |
|---|---|---|---|
| G5-01 | unit 静态验证 | `verify_rc=0`，用户、重启和关闭合同正确 | PASS |
| G5-02 | 正常启动 | active、gateway ready、请求与 MQTT 发布成功 | PASS |
| G5-03 | SIGTERM 停止 | 158 ms 内退出，最终 inactive | PASS |
| G5-04 | 直接 SIGINT | 135 ms 内退出，退出码 0 | PASS |
| G5-05 | 异常退出自动重启 | PID 更新，重启次数 +1，2752 ms 恢复 active | PASS |
| G5-06 | 无效配置 | 退出码 4，不进行无意义重启，最终 failed | PASS |
| G5-07 | 串口缺失 | 服务保持 active，无假成功；日志量和 CPU 有界 | PASS |
| G5-08 | broker 不可用 | 串口采集继续；MQTT 恢复连接并再次发布 | PASS |
| G5-09 | journal 可观测性 | 使用本 run cursor，必要生命周期事件齐全 | PASS |
| G5-10 | 重复启停 | 10/10 成功，PID、FD 和线程无漂移 | PASS |

所有场景记录的 `failures` 都为空，没有 missing、failed 或重复场景。

## 4. 正式证据检查

正式原始证据保存在树莓派私有 evidence 目录，不复制进公开仓库。run ID 为：

```text
20260814T181641Z_systemd_124724e0_001
```

核验结果：

- runner 真实退出码：0；
- `summary.status=PASS`；
- `environment_class=NATIVE_ELIGIBLE`；
- `expected_platform=linux-arm64`；
- `actual_platform=linux-arm64`；
- `actual_machine=aarch64`；
- `scenario_count=10`；
- `failed_scenarios=[]`；
- `missing_scenarios=[]`；
- `failures.json=[]`；
- `cleanup_ok=true`；
- `PASS` 存在；
- `IN_PROGRESS` 不存在；
- `sha256sum -c SHA256SUMS`：全部通过；
- 原始证据大小：约 1.2 MiB。

对应的公开安全投影见 [P3-S7-ARM64-T02_systemd_matrix.json](./P3-S7-ARM64-T02_systemd_matrix.json)。

## 5. 清理与主机状态

runner 完成后又从主机外部复核：

- `iot-gw` 用户和组不存在；
- 正式 unit、配置、固定二进制和状态目录不存在；
- TCP 18884 无监听；
- gateway、PTY 和 Python runner 进程不存在；
- `industrial_iot_gateway.service` 为 `LoadState=not-found`、`ActiveState=inactive`；
- systemd failed unit 基线没有新增项；
- 源码目录没有 `__pycache__`、`.pyc` 或 `.pyo`；
- 正式证据完成输入绑定后，约 25 MiB 的可重建 CP-F Release 构建目录已删除；
- 最终温度 49.1°C，`get_throttled=0x0`；
- 可用内存约 7.1 GiB，根分区可用空间约 109 GiB。

系统原有 Mosquitto 服务保持原状；验收使用的回环 18884 私有 broker 已退出，没有影响原有 1883 服务。

## 6. 能力声明边界

```text
ARM64_SYSTEMD_SCENARIOS=10/10_PASS
arm64_systemd_validated=true
arm64_software_integration_validated=true
hardware_validated=false
arm64_long_soak_validated=false
arm64_release_bundle_ready=false
```

当前仍不能宣称：

- 真实 USB-RS485、Modbus 传感器、STM32 或 CAN 通信已经验证；
- ARM64 已通过完整 8 小时长稳；
- ARM64 Release bundle 已制作或批准发布；
- x86_64 G4 结论可以自动继承到 ARM64。`g4_inheritance_status` 仍为 `REQUIRES_SEPARATE_REVIEW`。

## 7. 产物与审核边界

本工作块正式维护产物包括：

- [P3-S7-ARM64-T02_systemd_matrix.json](./P3-S7-ARM64-T02_systemd_matrix.json)；
- 本正式验收报告；
- [p3_s7_arm64_t02_树莓派systemd服务管理与十场景验收.md](./learning/p3_s7_arm64_t02_树莓派systemd服务管理与十场景验收.md)。

三份文档经用户审核通过后已同步到独立仓库的 `docs/` 和 `docs/learning/` 正式维护目录；CP-E 至 CP-F 实施记录继续保存在 Windows 规划目录。本轮没有执行 `git add`、`git commit` 或 `git push`，也没有改写 T01 当时的历史验收结论。
