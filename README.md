# 工业物联网网关

`industrial_iot_gateway` 是一个采用 C++17 开发的工业通信网关项目。

## 当前范围

仓库当前已实现：

- Modbus RTU CRC16、冻结子集编解码与流式解析；
- Linux `termios` 非阻塞串口、请求调度、超时/重试/退避、新鲜度与有界队列；
- 三从站 PTY 软件仿真和结构化 JSONL 证据；
- MQTT 3.1.1 QoS 1 上行、异步发布、有界缓存、按 topic 合并、重连、LWT 和指标；
- MQTT 可通过 `GATEWAY_ENABLE_MQTT` 独立关闭，关闭时保留原有 JSONL/PTY 路径。

当前不包含 MQTT 下行写寄存器、TLS/生产凭据、systemd、ARM64、真实 USB-RS485/STM32
硬件验收或 S4 故障注入 runner。

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

## 许可证状态

项目许可证尚未最终确定。MIT 只是在规划基线中获准保留的候选方案；在项目所有者确认前，不添加 `LICENSE` 文件。
