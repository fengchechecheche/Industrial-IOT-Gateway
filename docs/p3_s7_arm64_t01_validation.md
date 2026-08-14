# P3-S7-ARM64-T01：树莓派环境预检与原生构建验收报告

## 1. 验收结论

冻结候选提交：

```text
e62de0e946820c163c25fd7c82ebc3c4bc07afb5
```

正式证据 run：

```text
20260814T171411Z_arm64_native_e62de0e_003
```

最终判定：

```text
PASS_P3_S7_ARM64_T01_ENVIRONMENT_AND_NATIVE_BUILD
arm64_environment_eligible=true
arm64_native_build_pass=true
arm64_software_integration_validated=false
arm64_systemd_validated=false
arm64_release_bundle_ready=false
hardware_validated=false
```

本工作块已经证明冻结源码能够在 Raspberry Pi 4B 的 Ubuntu Server 24.04 ARM64 环境中原生
配置、编译、测试并形成可校验的 AArch64 Release 产物。它没有安装或启动正式网关服务，没有接入
真实 RS485/CAN 硬件，也没有把 ARM64 原生构建结果扩大为部署或硬件验收结论。

## 2. 验收范围

本次覆盖：

- 树莓派、操作系统、AArch64 用户空间、systemd、资源、电源和散热预检；
- Ubuntu 官方 ARM64 构建、测试、MQTT 和静态分析依赖；
- 冻结提交身份与传输归档哈希；
- MQTT OFF Debug、MQTT ON + PTY Debug、Release；
- ASan/UBSan、clang-tidy、clang-format、Python 和数据合同；
- 安装 `libclang-rt-18-dev` 后的 ARM64 TSan 条件门；
- AArch64 ELF、动态依赖、`DESTDIR` 安装暂存和 systemd unit 静态解析；
- 私有证据、SHA-256、Release handoff 和限定目录清理。

本次不覆盖：

- 正式 `systemctl enable/start`、开机自启和十场景 systemd 验收；
- USB-RS485、真实 Modbus 从站、STM32、CAN 或传感器；
- 树莓派长稳、断电恢复、真实串口重连和 ARM64 Release bundle 发布；
- v0.1.0 对 ARM64 或树莓派的公开支持声明。

## 3. 主要执行结果

| 验收门 | 结果 | 关键证据 |
|---|---|---|
| 环境与身份 | PASS | Raspberry Pi 4B、Ubuntu Server 24.04、AArch64、PID 1 为 systemd |
| 冻结源码 | PASS | 213 个候选文件，提交和归档 SHA-256 已固定 |
| MQTT OFF Debug | PASS | 179/179 CTest |
| MQTT ON + PTY Debug | PASS | 191/191 CTest |
| Release | PASS | 191/191 CTest，恢复证据重跑退出码 0 |
| ASan/UBSan | PASS | 191/191，诊断匹配数 0 |
| clang-tidy | PASS | 0 error；17 条既有非阻塞 warning；191/191 CTest |
| clang-format | PASS | 83/83 文件 |
| Python | PASS | 35/35 文件 |
| 数据合同 | PASS | 20/20 测试 |
| TSan | PASS | 安全探针 PASS；竞争预言机 PASS；24/24 定向、191/191 全量；项目诊断 0 |
| AArch64 二进制 | PASS | ELF/`readelf` 为 AArch64；`ldd` 无 `not found` |
| 安装暂存 | PASS | 9 个文件；路径、权限、大小和哈希一致；unit 静态解析退出 0 |
| 私有证据 | PASS | `failures.json=[]`；31 个正式文件通过 `sha256sum -c` |
| 清理与 handoff | PASS | 删除 16 个限定目标；无遗留进程；Release handoff 源/副本树一致 |

完整去敏矩阵见同目录下的 `P3-S7-ARM64-T01_build_matrix.json`。

## 4. CP8 二进制与安装暂存

对 `gateway_app`、`gateway_pty_bus`、`pty_slave` 和 `gateway_soak_driver` 执行 `file`、
`readelf -h` 和 `ldd`。四个文件均为 64 位 AArch64 ELF；动态依赖中没有 `not found`，Paho、
yaml-cpp、libstdc++ 和 libc 来自目标机 ARM64 系统库路径。

使用唯一 `/tmp` 根执行：

```bash
DESTDIR="<unique-stage>" cmake --install "<release-build>"
```

安装树共 9 个文件：`gateway_app` 为 `0755`，其余 unit、配置、示例环境文件和文档为 `0644`。
安装树综合哈希为：

```text
7403eecc6db54a80e4eb0c6e77b7e18b0c919180712980cb5aec04b4b5f6dbf1
```

`systemd-analyze verify --root=<unique-stage>` 返回 0。该操作只解析暂存根，没有写入
`/usr/local` 或 `/etc`，也没有创建用户、启用 unit 或启动服务。最终复核确认系统路径中的
`gateway_app`、项目配置目录和 `industrial_iot_gateway.service` 均不存在。

## 5. 执行中问题及处理

### 5.1 AArch64 TSan 运行库缺失

首次条件门因缺少 `libclang_rt.tsan-aarch64` 记录为 `NOT_RUN_TOOLCHAIN_BLOCKED`。用户安装
`libclang-rt-18-dev:arm64=1:18.1.3-1ubuntu1` 后重新执行：安全探针正常退出，故意竞争探针产生
TSan data-race 报告，随后项目构建和完整测试通过。首次阻塞记录保留，没有被覆盖成“从未失败”。

### 5.2 CP8 首轮证据脚本转义异常

首轮 `_001` 的安装和 unit 静态解析虽然返回 0，但 Windows 到 SSH 的转义使结构化 JSON 损坏。
该 run 没有用于验收，后续改用经过语法检查的临时 Python 证据生成器。

### 5.3 `ctest -N` 覆盖 `LastTest.log`

`_002` 自动矩阵发现 MQTT OFF 和 Release 的 `LastTest.log` 只有测试清单信息，无法独立证明历史
测试结果。未把它解释成代码失败，也没有手工改写 PASS；而是分别重跑 MQTT OFF 179/179 和
Release 191/191，Release 真实退出码为 0。正式 `_003` 在执行清单查询前复制六套完整
`LastTest.log`，由实际计数生成 PASS 矩阵。

## 6. 正式证据与清理

正式原始证据仅保存在树莓派私有 evidence 目录，未复制到公开仓库。关键哈希：

| 文件/对象 | SHA-256 |
|---|---|
| `SHA256SUMS` | `b9907d958a834f95482a8708df0369c592995f49faf48279020e399141ffd57e` |
| `summary.json` | `7d7c5b46c585795013be77f8d89ec939e8164076cc539e6dd93418dc0a010c1c` |
| `build_matrix.json` | `d61006ba753d6092ec4cab47618d0c2d6c92433d32cb1dced6c25a6bf0dc5f41` |
| `install_manifest.json` | `b883a2454d56848e694c2e51b49ccbe6c71c38c8395b287eeaa06719611b9da9` |
| Release handoff 树 | `a26972652e2567536978b3da8bfae7c29c1d1ec74212e72c966250c9062ae7fd` |

正式证据包含 31 个受哈希覆盖的文件，`failures.json` 为 `[]`。Release handoff 包含 150 个文件、
24,855,918 字节，源构建树与 handoff 树身份一致。

已删除六个可重建 Debug/ASan/clang-tidy/TSan 目录、两个探索 run、三个唯一 stage 和本轮临时
日志；保留正式 `_003` evidence、冻结候选源码、Release 原构建和 Release handoff。源码中的
`__pycache__`、`.pyc`、`.orig` 均为 0，没有遗留 CTest 或网关进程。

最终健康状态：可用内存约 7.2 GiB、可用磁盘约 109 GiB、SoC 约 47.2°C、
`get_throttled=0x0`。

## 7. 能力声明边界

当前可以准确表述：

> 工业通信网关冻结候选已在 Raspberry Pi 4B / Ubuntu Server 24.04 ARM64 上完成原生构建，
> 并通过 Debug、Release、ASan/UBSan、TSan、clang-tidy、格式检查、CTest、AArch64 ELF、动态
> 依赖和非系统安装暂存验收。

当前不能表述：

- 已完成树莓派生产部署；
- 已通过 ARM64 原生 systemd 十场景；
- 已在真实 RS485/CAN 总线上稳定运行；
- 已完成树莓派 8 小时长稳；
- v0.1.0 已正式支持 ARM64 或树莓派。

## 8. CP-D 固化状态

本报告和同步教程已经通过 CP-D 审核并同步到独立仓库正式文档目录。该同步只修改工作区文件；
未执行 Git 暂存、提交、推送、tag 或发布操作。
