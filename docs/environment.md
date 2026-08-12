# 开发环境

## 已验证环境

| 字段 | 内容 |
| ---- | ---- |
| 验证日期 | 2026-08-09 |
| WSL 发行版 | `Ubuntu-24.04-Gateway` |
| 操作系统 | Ubuntu 24.04.4 LTS |
| 架构 | x86_64 |
| 用户 | `iot-gw` |
| 仓库 | `/home/iot-gw/projects/industrial_iot_gateway` |
| CMake / CTest | 3.28.3 |
| GCC / G++ | 13.3.0 |
| Ninja | 1.11.1 |
| Clang | 18.1.3 |
| clang-format | 18.1.3 |
| clang-tidy | 18.1.3 |
| GoogleTest | 1.14.0 |
| yaml-cpp | 0.8.0 |
| glibc `openpty` / `libutil` | 2.39 |
| Eclipse Paho MQTT C++ | 1.2.0-2 |
| Eclipse Paho MQTT C | 1.3.13-1build2 |
| nlohmann/json | 3.11.3-1 |
| Mosquitto / clients | 2.0.18-1build3 |
| Python | 3.12.3（T03 runner 仅使用标准库） |

## 安装来源

`P3-S2-T01` 工具链通过已配置的 Ubuntu 24.04 软件源安装，使用的命令为：

```bash
apt-get update
apt-get install -y \
  build-essential cmake ninja-build \
  clang clang-format clang-tidy \
  libgtest-dev pkg-config
```

`P3-S2-T05` 新增 YAML 解析开发依赖：

```bash
apt-get update
apt-get install -y --no-install-recommends libyaml-cpp-dev
```

PTY 链路直接使用 glibc 提供的 `<pty.h>`、`openpty()` 和 `libutil`。本任务没有安装
`socat`、外部 Modbus 协议库或硬件专用依赖。

`P3-S4-T02` 的 MQTT ON 构建固定使用：

```bash
apt-get update
apt-get install -y --no-install-recommends \
  libpaho-mqttpp-dev=1.2.0-2 \
  libpaho-mqtt-dev=1.3.13-1build2 \
  nlohmann-json3-dev=3.11.3-1 \
  mosquitto=2.0.18-1build3 \
  mosquitto-clients=2.0.18-1build3
```

2026-08-09 首轮验收使用精确版本 `.deb` 临时解包完成。随后项目所有者执行了上面的固定
版本安装命令，`dpkg-query` 确认五个软件包均已安装且版本完全匹配。系统依赖复验没有设置
临时 `CMAKE_PREFIX_PATH` 或 `LD_LIBRARY_PATH`；CMake 直接从 `/usr/include` 和
`/usr/lib/x86_64-linux-gnu` 找到 Paho 与 nlohmann/json，MQTT ON 全目标构建通过，完整
CTest 为 168/168。`ldd` 同时确认 `gateway_app` 从系统库目录加载 Paho，未发现缺失动态库。
复验使用的 `/tmp/industrial_iot_gateway_t02_system_debug` 构建目录已在记录结果后删除。

## P3-S2-T01 验证状态

| 配置 | 配置生成 | 构建 | CTest |
| ---- | -------- | ---- | ----- |
| Debug + warnings as errors | PASS | PASS | PASS，1/1 |
| ASan + UBSan | PASS | PASS | PASS，1/1 |
| Release | PASS | PASS | PASS，1/1 |
| clang-tidy | PASS | PASS | PASS，1/1 |

## P3-S4-T02 验证状态

| 配置 | 构建 | CTest |
|---|---|---|
| Debug，MQTT OFF | PASS | PASS，158/158 |
| Debug，MQTT ON + Mosquitto + PTY | PASS | PASS，168/168 |
| ASan + UBSan，MQTT ON | PASS | PASS，168/168 |
| Release，MQTT ON | PASS | PASS，168/168 |
| clang-tidy，MQTT ON | PASS | 全目标构建完成 |
| TSan，MQTT ON + WSL2 workaround | PASS | PASS，168/168 |
| clang-format 18 | PASS | `--dry-run --Werror` |

安装固定版本系统依赖后，另行执行了一次不使用临时依赖路径的 Debug、MQTT ON、Mosquitto
和 PTY 干净复验：配置、全目标构建和 CTest 均通过，CTest 结果为 168/168。

## P3-S4-T03 验证状态

T03 引入 Python 标准库故障矩阵 runner、F01–F15 软件场景、慢 PUBACK 代理、动态 PTY
故障切换以及请求/测量队列的 fail-fast 和满队列可观测性。固定版本系统依赖保持不变，
未安装额外 Python 包。最终复验结果如下：

| 配置 | 构建 | CTest / 场景结果 |
|---|---|---|
| Debug，MQTT ON + PTY + Mosquitto | PASS | PASS，176/176 |
| ASan + UBSan，MQTT ON | PASS | PASS，176/176 |
| Release，MQTT ON | PASS | PASS，176/176 |
| clang-tidy 18，MQTT ON | PASS | 全目标构建完成；T03 新增诊断已清理 |
| TSan，MQTT ON + WSL2 workaround | PASS | PASS，176/176 |
| clang-format 18 | PASS | 全仓 `--dry-run --Werror` |
| 软件故障矩阵 | PASS | F01–F15，15/15，未关闭失败 0 |

正式 G3 故障矩阵证据位于
`artifacts/baseline/20260809T155317Z_g3_b293e40_001/`。该次运行在 manifest 中明确标记
`exploratory=false`，`source_revision` 为
`b293e402fe3b45e60de3c8f09512378c959f4bbd`。F01–F15 为 15/15 PASS，
`unclosed_failures=0`，矩阵总耗时 7522 ms，全部文件通过
`sha256sum -c SHA256SUMS`。因此，绑定提交版本的正式 G3 基线状态为 PASS。

TSan 首轮 175/176，F11 暴露 MQTT worker 读取 `drain_deadline_` 与主线程写入之间的真实
竞态；使用专用互斥量保护截止时间，并在设置截止时间后再发布停止标志，定向与全量复测均
通过。

以上 MQTT 验收使用回环地址上的匿名 Mosquitto，只证明本地软件链路。它不证明 TLS、生产
认证、跨主机网络、真实 RS485 电气层或硬件台架行为。

`P3-S2-T01` 的历史基线仍保留，但不再代表仓库当前能力上限。

## P3-S5-T01 验证状态

2026-08-12 在系统依赖不变的条件下完成 S5 软件故障矩阵复核。历史 S4 正式 G3 基线通过
结构、事件追踪和 68 项 SHA256 复核。正式运行
`20260812T134119Z_g3_66a9c95_001` 绑定提交
`66a9c95154c4760e8c4e061867d3c0133a6cb60b`，F01–F15 为 15/15 PASS、未关闭失败 0，并正确
记录 `exploratory=false`、`gate=G3`、`stage=S5`、`task=P3-S5-T01`。

质量门在最终预检代码上得到：Debug、ASan/UBSan、Release、TSan 均为 176/176 PASS，
clang-tidy 全目标构建完成，clang-format、Python 语法和 JSON 解析通过。TSan 首轮在 F10
捕获 Paho publish delivery token 的完成回调与 token 析构并发；通过有界保留两代 token，
F10 在 TSan 下连续三次定向通过，随后全量 TSan 176/176。

正式证据包含 968 条事件和 68 项 SHA256，独立审计未发现结构、追踪或校验错误。F06、F10、
F11、F13 的恢复观察值分别为 20、1450、20、20 ms；这些是单次本地观察值，不是生产 SLA。
本次 P3-S5-T01 结论为 PASS，P0/P1/P2 均为 0，P3-S5-T02 记为 `NOT_TRIGGERED`。

## P3-S5-T03 验证状态

T03 在相同系统依赖上新增标准库 Python soak runner、C++ soak driver、共享 PTY 仿真库、
profile/evidence schema、`/proc` 资源采样和分段日志；没有安装额外软件包或 Python 第三方包。

| 配置 | 构建 | CTest / 检查结果 |
|---|---|---|
| Debug，MQTT ON + PTY + Mosquitto | PASS | PASS，181/181 |
| ASan + UBSan，MQTT ON | PASS | PASS，181/181 |
| Release，MQTT ON | PASS | PASS，181/181 |
| clang-tidy 18，MQTT ON | PASS | 全目标构建完成；T03 driver 新增诊断已清理 |
| TSan，MQTT ON + WSL2 workaround | PASS | PASS，181/181 |
| clang-format 18 | PASS | 全项目 `--dry-run --Werror` |
| Python / JSON / soak 单测 | PASS | `py_compile`、JSON 解析、10/10 |
| 90 秒 soak smoke | PASS | 4/4 soak CTest；七类故障均触发和恢复 |

最终 smoke 只验证执行工具、逐循环 oracle、资源采样、broker 断线、PTY 重连、日志轮转和有界
清理，`long_soak_pass=false`。T04 后续已经执行但结论为 FAIL；S6 的 28800 秒 release 尚未
执行，因此当前没有软件长稳 PASS、树莓派部署或硬件长稳结论。

## P3-S5-T04 验证状态

2026-08-13 完成绑定提交 `e38390ee560fa0573b3aebf16ccc6fc225361400` 的 3600 秒软件
preflight。正式 run ID 为 `20260812T153917Z_soak_preflight_e38390e_001`，runner 完整运行
并有界收尾，全部证据通过 `sha256sum -c SHA256SUMS`，但最终
`summary.status=FAIL`、`unclosed_failures=1`。

唯一机器失败为 P1 `SOAK-001`：650 ms 延迟响应故障期间，非目标从站最大成功间隔 13.5 秒，
超过冻结的 10 秒公平性门限。资源与性能门通过：正常成功率 100%、吞吐 26.201 requests/s、
RSS 峰值 9.941 MiB、RSS 斜率 0.444 MiB/hour、CPU 平均 2.511%、fd 峰值 12、线程峰值 11；
七类故障均触发并恢复。

日志轮转通过，最大段 67,107,966 bytes，没有超过 64 MiB；但按末尾实测约 42,576.75 bytes/s
线性外推，8 小时证据约 1.142 GiB，预计约 7.0 小时触碰 1 GiB 单次证据上限。必须先关闭公平性
缺陷并形成版本化容量决策，再绑定新提交完整重跑 preflight。当前 S6 被阻塞。

### T04 阻塞项修复状态

2026-08-13 已完成候选修复：10 秒公平性门限保持不变；运行时新增 200 ms 有界晚响应隔离，
丢弃字节后等待 RTU 3.5 字符静默再恢复调度；soak 摘要强制每个 delayed 循环出现
`late_response_discarded`。新建 `software_release_v2` / `software_preflight_v2`，证据上限调整为
5 GiB，启动/运行时磁盘余量调整为 10/5 GiB，v1 和首次 FAIL 证据保留。

Debug、ASan/UBSan、Release、TSan 均为 182/182 PASS；clang-tidy 全目标完成，本次新增代码无
新增诊断；clang-format、Python、JSON 和 v2 validate-only 通过。正式 v2 preflight 尚未绑定
新提交运行，因此当前状态是“修复实现与质量门 PASS、最终 T04 待复跑”，S6 仍被阻塞。
