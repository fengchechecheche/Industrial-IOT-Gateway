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
