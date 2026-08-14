# P3-S7-ARM64-T02：树莓派 systemd 服务管理与十场景验收

> 文档性质：中文教程正式维护版  
> 对应实现：候选提交 `124724e00f6ef2c9d03323198dd618a0af1ae7cd`  
> 对应正式结果：ARM64 原生 systemd 十场景 10/10 PASS  
> 适用环境：Raspberry Pi 4B、Ubuntu Server 24.04 ARM64、systemd 255

## 1. 本工作块解决什么问题

前面的 PTY、MQTT 和长稳测试证明了网关程序自身的大部分行为，但实际部署时还需要回答另一组问题：系统启动服务时用哪个用户？程序崩溃后是否自动恢复？收到停止命令时能否在限定时间退出？日志怎样与某一次运行关联？安装的文件和测试用户能否安全清理？

P3-S7-ARM64-T02 把候选程序放到树莓派原生 systemd 环境中，使用真实 PID 1、真实 service unit 和真实 journal 完成十场景验收。它验证的是 **ARM64 软件部署和服务生命周期**，不是 RS485/CAN 电气层或真实传感器。

## 2. 基础术语

- **PID 1**：Linux 启动后第一个用户空间进程。Ubuntu Server 24.04 上是 systemd。
- **unit**：systemd 管理对象的配置单元；本项目使用 service unit。
- **service manager**：启动、停止、重启和观察服务状态的 systemd 管理器。
- **main PID**：systemd 认定的服务主进程 PID。
- **journal**：systemd-journald 管理的结构化系统日志。
- **journal cursor**：日志位置标识，用于只读取某个起点之后的记录。
- **StateDirectory**：systemd 为服务创建并管理的持久状态目录。
- **PTY（Pseudo Terminal）**：伪终端；本项目用它模拟串口链路。
- **oracle（测试预言机）**：把实际观测值转换成 PASS/FAIL 的确定性规则。
- **fixture（测试夹具）**：为测试提供输入和环境的辅助进程；这里包括 PTY 总线和回环 MQTT broker。

## 3. PID 1 与 systemd 的角色

直接在终端运行 `gateway_app` 时，终端用户负责进程生命周期。部署成 service 后，systemd 负责：

1. 按 unit 指定的身份和目录启动程序；
2. 记录主进程 PID、退出码和状态；
3. 按策略决定是否重启；
4. 在停止时发送信号并执行超时约束；
5. 把标准输出和错误输出收集到 journal；
6. 应用目录、权限、能力和系统调用面的安全限制。

因此“程序手工运行成功”不能替代“service 行为验证通过”。本工作块必须在 `systemd-detect-virt=none` 的物理树莓派上执行，不能把 WSL、容器或 VMware 的结果冒充树莓派原生结论。

## 4. 本项目 unit 的三个部分

### 4.1 `[Unit]`：依赖与启动限制

`Wants=network-online.target` 和 `After=network-online.target` 表示服务希望网络已在线，并在它之后启动。`StartLimitIntervalSec=60`、`StartLimitBurst=5` 限制短时间内反复失败导致的重启风暴。

这并不保证 MQTT broker 一定可用。网络在线只说明主机网络达到相应 target，broker 故障仍由应用重连机制处理。

### 4.2 `[Service]`：进程如何运行

关键配置与作用如下：

| 配置 | 本项目含义 |
|---|---|
| `Type=simple` | `ExecStart` 启动的前台进程就是服务主进程 |
| `User/Group=iot-gw` | 避免以 root 身份运行网关 |
| `SupplementaryGroups=dialout` | 为后续真实串口访问预留设备组权限 |
| `StateDirectory=industrial_iot_gateway` | 由 systemd 管理 `/var/lib/industrial_iot_gateway` |
| `EnvironmentFile=...` | 串口、寄存器表和 MQTT 参数由部署环境提供 |
| `Restart=on-failure` | 非正常退出时自动重启 |
| `RestartSec=2s` | 重启前等待 2 秒，避免紧密循环 |
| `RestartPreventExitStatus=2 3 4 5 6 7` | 已知配置/启动类错误不重复重启 |
| `TimeoutStopSec=5s` | 停止总预算为 5 秒 |
| `KillSignal=SIGTERM` | 正常停止首先发送 SIGTERM |
| `StandardOutput/Error=journal` | 日志统一进入 journal |

`NoNewPrivileges`、`ProtectSystem=strict`、`ProtectHome=true`、空的 capability 集合等配置构成最小权限边界。它们的目标不是让服务“绝对安全”，而是减少网关进程被利用后能修改或读取的系统范围。

### 4.3 `[Install]`：是否随系统目标启用

`WantedBy=multi-user.target` 描述将来执行 `systemctl enable` 时应创建什么依赖关系。本次验收只测试临时安装和运行，**没有执行 enable**，所以不会建立开机自启链接。

## 5. 退出码、信号和重启策略

本项目把“可恢复的运行时故障”和“重启也解决不了的启动错误”分开：

- 进程异常崩溃属于 `on-failure` 的恢复范围；
- 无效配置返回退出码 4，命中 `RestartPreventExitStatus`，systemd 不应无限重启；
- `systemctl stop` 发送 SIGTERM，程序应进入有界关闭流程；
- 直接 SIGINT 用来验证应用自己的信号处理和退出路径，不等同于 systemd 的标准停止操作。

这就是为什么“无效配置后服务处于 failed”在 G5-06 中反而是正确结果：验收关注的是错误有没有被准确分类，而不是要求所有场景最后都显示 active。

## 6. StateDirectory 与服务用户

固定服务用户 `iot-gw` 用于隔离权限。`StateDirectory=industrial_iot_gateway` 使 systemd 在服务启动时准备工作目录，并按 unit 设定的模式管理权限。

本次 runner 在开始前先检查同名用户、组、unit、配置、二进制和状态目录是否已经存在。只要发现碰撞就停止，因为已有对象可能属于真实部署，测试不能猜测所有权并覆盖它。

runner 只删除自己在本次 run 中登记创建的对象。这个“所有权登记”原则比简单的 `rm -rf` 更重要：自动化清理必须能证明目标由本次任务创建，且路径位于固定允许范围。

## 7. journal cursor 为什么重要

如果直接执行 `journalctl -u industrial_iot_gateway.service`，可能读到此前启动留下的日志，从而把旧的 `gateway_ready` 误当成本次成功证据。

runner 在场景开始前保存 journal cursor，随后使用 `--after-cursor` 读取本次场景之后的日志。G5-09 不仅检查事件名称，还要求 `cursor_scoped=true`。这样才能证明 `runtime_started`、`gateway_ready`、`stop_requested` 和 `gateway_summary` 来自当前 run。

结构化日志中的事件名相当于稳定接口。测试不依赖自然语言句子，而是依赖机器可识别的生命周期事件，从而减少文本改写带来的误判。

## 8. PTY 和 MQTT 测试夹具

### 8.1 PTY 串口仿真

PTY 具有 master/slave 两端。`gateway_pty_bus` 持有一端并模拟 Modbus RTU 从站，`gateway_app` 把另一端当作串口设备。内核负责双向字节传输，因此可以验证 termios、请求调度、CRC、超时和关闭路径。

PTY 没有 RS485 差分电压、A/B 线、终端电阻、偏置、共模范围、USB-RS485 驱动和电磁干扰，所以 PTY PASS 不能推导出物理总线 PASS。

### 8.2 回环 MQTT broker

runner 使用回环地址上的独立 18884 端口，避免影响系统已有 Mosquitto。G5-08 暂停或移除测试 broker 时，网关仍应继续串口采集；broker 恢复后应重新连接并再次成功发布。

这验证应用层的断线恢复，但没有验证生产网络、TLS、认证、远程 broker 或防火墙策略。

## 9. 十场景分别证明什么

| 场景 | 主要问题 | 冻结判据摘要 |
|---|---|---|
| unit_static_verify | unit 是否可解析且合同正确 | verify=0；用户、Restart、超时等属性一致 |
| normal_start | 正常服务能否工作 | active、PID 有效、ready、请求和 MQTT 成功数增加 |
| sigterm_stop | 标准停止是否有界 | 5 秒内停止、PID 清零、最终 inactive |
| sigint_direct | 应用信号路径是否正确 | 退出码 0，5 秒内退出 |
| abnormal_restart | 崩溃后能否恢复 | PID 改变、只增加一次重启，1.5～5 秒恢复 |
| invalid_config | 配置错误会不会重启风暴 | 退出码 4、重启增量 0、最终 failed |
| missing_serial | 串口缺失是否忙等或伪成功 | 打开成功数 0，CPU ≤1000 ms，日志 ≤64 KiB |
| broker_unavailable | MQTT 故障会不会阻塞采集 | 串口成功数增加，失败计数有界，恢复后发布成功 |
| journal_observability | 日志能否支撑追踪 | 当前 cursor 范围内必要事件齐全 |
| repeated_cycles | 重复启停是否泄漏 | 10 次全成功，无残留 PID，FD/线程漂移 ≤2 |

本次真实观测包括：正常窗口 108 次请求成功和 111 次 MQTT 发布成功；异常重启 2752 ms；串口缺失窗口 CPU 增量 74 ms、日志 7974 字节；broker 故障期间仍完成 255 次串口成功；重复启停的 PID、FD 和线程漂移均为 0。

## 10. runner 的执行与证据流程

代码中的主要角色是：

- `NativeInputs`：保存目标平台、候选提交和所有冻结输入路径；
- `NativeG5Runner`：执行碰撞检查、部署、十场景、证据记录和清理；
- `evaluate_scenario()`：调用对应 oracle，把 observation 转换为场景记录；
- `EvidenceStore`：分配唯一 run 目录，写事件、records、summary、failures 和哈希；
- `environment_evidence()`：记录板卡、OS、内核、架构、PID 1、温度、电源、内存和磁盘；
- `input_evidence()`：记录候选提交、依赖版本和输入文件 SHA-256。

整体顺序是：

```text
平台与输入验证
  -> 固定对象和端口碰撞检查
  -> 创建唯一 IN_PROGRESS run
  -> 临时部署服务用户、unit、配置和二进制
  -> 启动 PTY/MQTT 测试夹具
  -> 逐场景采集 observation 并执行 oracle
  -> finally 清理本次登记对象
  -> 生成 records/summary/failures/SHA256SUMS
  -> 全部通过后删除 IN_PROGRESS 并写入 PASS
```

`EvidenceStore.finalize()` 只有在场景汇总为 PASS、清理成功且失败列表为空时才创建 PASS，并返回 0；否则 runner 返回 4。人工阅读“看起来正常”的日志不能替代这个机器判定。

## 11. 为什么还要在 runner 外复核

`cleanup_ok=true` 是 runner 的内部结论，但测试框架自身也可能有缺陷。因此 CP-H 又从 runner 外部检查：用户/组、固定文件、状态目录、18884、进程、failed units、温度和限频状态。

同时执行 `sha256sum -c SHA256SUMS`，用于发现证据生成后被意外修改的情况。SHA-256 证明文件内容与清单一致，但不证明测试设计本身正确，所以仍需场景 oracle、输入绑定和人工审核共同形成可信结论。

## 12. 常见错误和安全边界

- 把“能手工运行”写成“systemd 已验证”；
- 在 VM 或 WSL 中使用绕过参数后宣称物理树莓派 PASS；
- 只检查 9/10 场景就创建 PASS；
- 无效配置后要求服务反复自动重启；
- 不使用 journal cursor，混入旧日志；
- 发现已有 `iot-gw` 或固定 unit 后仍覆盖；
- 使用生产 broker 或真实串口做软件验收夹具；
- 清理时扩大到未知目录或删除已有系统服务；
- 修改 `summary.json`、删除失败记录后重新计算哈希；
- 把 ARM64 systemd PASS 扩大成硬件、长稳或发布包已完成。

## 13. 本工作块完成与未完成的能力

已经完成：

- 树莓派 ARM64 原生 Release 构建和 191/191 CTest；
- 原生 PID 1/systemd 服务运行；
- 十场景 10/10 PASS；
- unit、重启、信号、journal、PTY/MQTT 故障和重复启停验证；
- 证据哈希与系统对象清理复核。

仍未完成：

- 真实 RS485 Modbus 从站和环境传感器；
- STM32/Waveshare Shield 与 CAN；
- ARM64 8 小时长稳；
- ARM64 Release bundle 制作和发布；
- 开机自启实际演练。此次明确没有执行 `systemctl enable` 或重启。

## 14. 后续学习资料

以下资料均为一手或上游文档，访问日期为 2026-08-15：

- [systemd.service 手册（systemd 255）](https://www.freedesktop.org/software/systemd/man/255/systemd.service.html)：服务类型、Restart、TimeoutStopSec 和退出状态。
- [systemd.exec 手册（systemd 255）](https://www.freedesktop.org/software/systemd/man/255/systemd.exec.html)：User、StateDirectory、权限和沙箱选项。
- [systemd.unit 手册（systemd 255）](https://www.freedesktop.org/software/systemd/man/255/systemd.unit.html)：Unit/Install、依赖和启动限制。
- [journalctl 手册（systemd 255）](https://www.freedesktop.org/software/systemd/man/255/journalctl.html)：unit 过滤、cursor 和日志输出。
- [Linux Kernel TTY 文档](https://www.kernel.org/doc/html/v5.17/tty/index.html)：TTY/PTY 内核结构和工作方式。
- [Linux Kernel 设备文档中的 PTY 说明](https://www.kernel.org/doc/html/v5.15/admin-guide/devices.html)：PTY master/slave 和 `/dev/pts`。
- [OASIS MQTT 3.1.1 + Errata 01](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html)：发布订阅、QoS、会话和断线行为。

阅读时应以树莓派实际安装的 systemd 255 为主要版本；更高版本手册可能包含当前系统尚不支持的选项。
