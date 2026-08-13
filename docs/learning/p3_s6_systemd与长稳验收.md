# P3-S6｜systemd 与长稳验收教程

## 1. 本工作块解决什么问题

前面的工作块已经证明协议、线程、PTY 和 MQTT 功能可以运行。P3-S6 继续回答两个工程问题：
程序持续运行 8 小时后是否仍满足资源、数据质量和恢复门限；以及 Linux 把它当作长期服务管理时，
启动、停止、崩溃重启和配置错误是否可预测。

## 2. 术语和基础概念

- daemon/service：不依赖交互终端、长期运行的后台程序。
- service manager：负责服务生命周期、依赖、身份和日志的管理器；本项目使用 systemd。
- unit：systemd 的声明式配置文件，本项目是 `industrial_iot_gateway.service`。
- main process：systemd 追踪的主进程，即 `gateway_app`。
- graceful shutdown：先通知线程停止、关闭队列、排空 MQTT、join 线程，再退出。
- restart storm：持续失败的配置被反复快速拉起，消耗 CPU、日志和运维注意力。
- soak test：让系统在真实负载与周期故障下长时间运行，并由机器 oracle 判定。
- candidate Release：通过当前软件门的候选包，不自动等同生产发布。

## 3. systemd 如何管理本项目

`Type=simple` 表示 `ExecStart` 启动的进程就是主进程。systemd 以 `iot-gw` 身份运行它，
从 `/etc/industrial_iot_gateway/gateway.env` 读取串口、寄存器表和可选 MQTT 参数，并把
stdout/stderr 收进 journal。

正常停止时，`KillSignal=SIGTERM` 触发主程序的同步信号等待器。主线程调用
`GatewayRuntime::request_stop()` 和 `join()`；调度、串口、质量和 MQTT worker 依次结束。
`TimeoutStopSec=5s` 是 systemd 的外部上限，项目内部 MQTT drain 仍只有 2 秒。

`Restart=on-failure` 处理进程被 SIGKILL 或崩溃等异常；`RestartSec=2s` 避免立即高频拉起。
`RestartPreventExitStatus=2 3 4 5 6 7` 表示应用已经分类并主动返回的参数、信号初始化、配置、
启动、写请求和关闭错误不自动重启。错误配置因此 fail-fast，而不是制造 restart storm。

## 4. 缺失串口为什么不退出

寄存器表错误属于静态配置错误，重试不会自行变好，所以主程序返回 4。串口设备缺失可能是
USB-RS485 临时拔出或设备枚举尚未完成；运行时会保持服务 active，在串口线程内按
`serial_reopen_backoff` 有界重试。systemd 不需要重启整个进程，MQTT、队列和线程所有权也
不会反复重建。

这两种错误必须使用不同 oracle：前者检查“退出一次且不重启”，后者检查“进程存活、没有忙循环、
无 systemd restart、收到 SIGTERM 能有界停止”。

## 5. broker 不可达为什么不能阻塞串口

`MqttPublishSink` 有独立 worker。串口和调度线程只向有界 publish 队列提交消息，不在持有
运行时锁时执行网络连接。broker 不可达时，MQTT worker 记录 `mqtt_connect_failed` 并执行
指数退避；串口线程继续轮询。关闭时 MQTT 队列最多排空 2000 ms，超过预算的数据按合同计数，
不能拖过 systemd 的 5000 ms 总预算。

## 6. 8 小时数据流和判定

`run_soak.py` 启动本地 broker、subscriber 和 Release driver；C++ driver 内运行真实
`GatewayRuntime` 与三从站 PTY harness。runner 每约 5 秒采样 `/proc` 中的 RSS、CPU、
fd 和线程数，并按 64 MiB 轮转日志。八个一小时循环各注入七类故障。

结束后 `summarize_soak.py` 划分 warm-up、正常、故障和恢复窗口，计算成功率、吞吐、尾延迟、
公平性、队列、MQTT、RSS 趋势和证据容量。212 个强制 oracle 任一失败都会让进程返回非零，
不能靠人工看到“大多数正常”而判 PASS。

## 7. 关键文件与执行路径

- `packaging/systemd/industrial_iot_gateway.service`：身份、路径、重启、停止和加固合同；
- `packaging/systemd/gateway.env.example`：不含凭据的公开配置模板；
- `tests/systemd/test_systemd_unit.py`：冻结 unit 和 CMake 安装规则；
- `src/app/main.cpp`：参数解析、退出码、SIGINT/SIGTERM 等待和最终摘要；
- `src/runtime/gateway_runtime.cpp`：线程启动、串口 reopen、停止和 join；
- `src/mqtt/mqtt_publish_sink.cpp`：异步连接、退避、有界缓存和 drain；
- `tools/soak/runner.py`、`summary.py`：长稳编排与自动判定；
- `CMakeLists.txt`：把 executable、unit 和环境模板放入安装树。

## 8. 为什么选择当前方案

- 使用专用非 root 用户，把串口权限单独交给 `dialout`，缩小服务权限；
- 明确工作目录和绝对配置路径，避免 systemd 默认环境与交互 shell 不同；
- 配置错误与运行期瞬态错误分流，避免“所有错误都重启”；
- journal 保留 unit、时间、PID 和退出状态，便于把应用 JSONL 与服务生命周期关联；
- CMake `install()` + `DESTDIR` 生成可审计的暂存树，不依赖人工散落复制；
- 软件长稳和 systemd 场景分开保存证据，避免一个 PASS 掩盖另一层缺口。

## 9. 常见错误、失败模式和安全边界

- 把 `Restart=always` 用于错误配置，造成重启风暴；
- 以 root 运行只为绕过串口权限，而没有配置 `dialout`；
- 在 unit 中依赖相对路径、shell profile 或未定义环境变量；
- SIGTERM handler 内执行复杂锁操作，导致关闭死锁；本项目使用同步信号等待线程；
- broker 断线时在串口线程同步 reconnect；
- 只看服务是 active，不检查是否仍有成功采集和数据质量；
- 把 WSL2 systemd 结果升级成 Raspberry Pi ARM64 结论；
- 把 PTY 的毫秒级延迟当成真实 RS485 性能。

`ProtectSystem=strict`、`NoNewPrivileges=true` 等只是基线加固，不替代最小权限审计、凭据管理、
更新策略或发行版沙箱测试。当前 unit 允许 AF_UNIX/AF_INET/AF_INET6，因为 MQTT 需要网络。

## 10. 已完成与未完成能力

已完成：8 小时软件长稳、212/212 oracle、systemd 静态校验、非 root、SIGTERM、有界关闭、
SIGKILL 自动重启、错误配置 fail-fast、串口缺失退避、broker 断线降级、三轮启停、CMake 安装
和 x86_64 RC。

未完成：ARM64/树莓派复验、开机真实自启动、udev 稳定串口名、USB-RS485/STM32/传感器实测、
生产 TLS/凭据、真实温度/降频与硬件长稳。

## 11. 后续学习资料

以下均为一手资料，适用版本为 systemd 255、CMake ≥3.22、Linux/Python 当前项目环境，访问日期
为 2026-08-13：

- [systemd.service](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)：
  `Type`、`Restart`、`RestartPreventExitStatus` 和停止语义；
- [systemd.exec](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html)：
  用户、组、目录、环境、日志与沙箱选项；
- [systemd.unit](https://www.freedesktop.org/software/systemd/man/latest/systemd.unit.html)：
  依赖、启动限速与 unit 生命周期；
- [journalctl](https://www.freedesktop.org/software/systemd/man/latest/journalctl.html)：
  按 unit、时间和字段筛选日志；
- [CMake install](https://cmake.org/cmake/help/latest/command/install.html) 与
  [DESTDIR](https://cmake.org/cmake/help/latest/envvar/DESTDIR.html)：安装规则和打包暂存；
- [Linux proc 文件系统](https://docs.kernel.org/filesystems/proc.html)：RSS、线程与进程资源；
- [Clang ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html)：并发数据竞争边界。
