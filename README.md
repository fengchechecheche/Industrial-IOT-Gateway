# 工业物联网网关

`industrial_iot_gateway` 是一个采用 C++17 开发的工业通信网关项目。

## 当前范围

仓库当前已实现：

- Modbus RTU CRC16、冻结子集编解码与流式解析；
- Linux `termios` 非阻塞串口、请求调度、超时/重试/退避、新鲜度与有界队列；
- 三从站 PTY 软件仿真和结构化 JSONL 证据；
- MQTT 3.1.1 QoS 1 上行、异步发布、有界缓存、按 topic 合并、重连、LWT 和指标；
- MQTT 可通过 `GATEWAY_ENABLE_MQTT` 独立关闭，关闭时保留原有 JSONL/PTY 路径。
- 绑定冻结提交的 8 小时软件长稳、周期故障、资源趋势和 SHA-256 自动验收；
- 非 root systemd unit、错误配置 fail-fast、串口缺失退避、异常重启和 CMake 安装暂存。
- 在 VMware Ubuntu Server 24.04 x86_64 与 Raspberry Pi 4B Ubuntu Server 24.04
  ARM64 上形成同一候选提交的 v0.1.0 本地双架构软件 Release；
- 两个平台均通过原生 Release 构建、完整 CTest、确定性双归档、清洁解包、
  PTY/Mosquitto 演示、SIGTERM 和卸载复演；
- Raspberry Pi 4B ARM64 已通过 8 小时软件长稳与受控 systemd 重启自启演练；
- Ubuntu-24.04-Gateway x86_64 已通过新 CH340 USB-RS485，以只读地址 4 profile
  连续读取真实项目五 STM32 节点，并完成 JSONL、一次 NUCLEO RESET 恢复和约 60 秒
  本地 Mosquitto 投影。详见 [`docs/p3_p5_modbus_interop.md`](docs/p3_p5_modbus_interop.md)。

当前已验证 Linux x86_64 与 Raspberry Pi 4B ARM64 的软件构建、测试、systemd、长稳和
本地 bundle；另在 Ubuntu-24.04-Gateway x86_64、新 CH340、短线共地和地址 4 边界内完成
一次真实 STM32 Modbus/RS485 上行联调。但仍未创建 Git tag、未上传 GitHub Release，也未
验证 Raspberry Pi 真实 RS485、商用 Modbus 从站、项目三 CAN、电气安全或硬件长稳。
PTY/Mosquitto 软件证据和本次窄范围台架结果都不能外推为完整 G6 或工业现场验收。当前也不
包含 MQTT 下行写寄存器、TLS 或生产凭据。部署与排障命令见 `docs/runbook.md`。

## 构建与测试

```bash
cmake -S . -B build/debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_BUILD_TESTS=ON \
  -DGATEWAY_BUILD_PTY_SLAVE=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

MQTT ON 依赖 Ubuntu 24.04 固定版本软件包：

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  libpaho-mqttpp-dev=1.2.0-2 \
  libpaho-mqtt-dev=1.3.13-1build2 \
  nlohmann-json3-dev=3.11.3-1 \
  mosquitto=2.0.18-1build3 \
  mosquitto-clients=2.0.18-1build3
```

启用 MQTT 后构建：

```bash
cmake -S . -B build/mqtt-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_BUILD_TESTS=ON \
  -DGATEWAY_BUILD_PTY_SLAVE=ON \
  -DGATEWAY_ENABLE_MQTT=ON
cmake --build build/mqtt-debug
ctest --test-dir build/mqtt-debug --output-on-failure
```

本地 broker 运行示例只用于回环验收：

```bash
mosquitto -p 1883
./build/mqtt-debug/gateway_app \
  --serial-device /dev/pts/0 \
  --register-map config/examples/register_map.yaml \
  --mqtt-broker-uri tcp://127.0.0.1:1883 \
  --mqtt-client-id iiotgwLab01 \
  --gateway-id lab_gateway_01
```

三个 MQTT 参数必须同时提供；省略整组参数时，即使二进制启用了 MQTT，也继续使用 JSONL
发布器。`client_id` 采用不超过 23 字符的字母数字标识，`gateway_id` 允许下划线。

## 一键软件演示

Ubuntu 24.04 Linux x86_64 安装上述固定依赖后，可从仓库根目录执行：

```bash
python3 tools/demo.py --profile pty-mqtt
```

该命令自动配置 Release 构建、编译已有 MQTT+PTY 集成目标并运行一个回环 Mosquitto、三从站
PTY、broker 断线/恢复和有界停止场景。成功输出包含 `DEMO_RESULT=PASS` 和
`HARDWARE_VALIDATED=false`。

这只是 Linux x86_64 上的软件集成演示，不代表树莓派、ARM64、systemd 稳定版门、
真实 USB-RS485、STM32、CAN 或 RS485 电气层已经验证。演示只清理由本次命令创建的进程，不会按
进程名停止用户已有的 broker。

## 本地双架构 Release

v0.1.0 本地 Release 集包含 Linux x86_64 与 Linux ARM64 两个软件包。两个包绑定同一产品
候选提交，并分别在原生平台完成构建、测试、确定性归档和 clean smoke。使用前应先验证
`SHA256SUMS`，再选择与 `uname -m` 匹配的软件包；详细步骤见 `docs/runbook.md`。

Release 资产保存在项目规划产物目录，不把原始 evidence、私有主机路径或测试凭据提交到
公共仓库。当前发布边界固定为：

```text
PUBLISHED=false
HARDWARE_VALIDATED=false
TAG=null
```

这表示本地双架构软件 Release 已就绪，但没有创建 Git tag 或 GitHub Release，也没有完成
完整 G6 硬件验收。当前仅新增一次 Ubuntu-24.04-Gateway x86_64、新 CH340、地址 4 的真实
STM32 只读联调；`HARDWARE_VALIDATED=false` 继续覆盖 Raspberry Pi、商用从站、CAN、
电气安全和硬件长稳等未完成范围。

Sanitizer 构建：

```bash
cmake -S . -B build/sanitizer -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_BUILD_TESTS=ON \
  -DGATEWAY_ENABLE_SANITIZERS=ON
cmake --build build/sanitizer
ctest --test-dir build/sanitizer --output-on-failure
```

Release 构建：

```bash
cmake -S . -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGATEWAY_BUILD_TESTS=ON
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

## 许可证

项目自有代码采用 [MIT License](LICENSE)，版权行是
`Copyright (c) 2026 fengchechecheche`。第三方依赖保持各自许可证，详见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和
[docs/third_party_inventory.md](docs/third_party_inventory.md)。
