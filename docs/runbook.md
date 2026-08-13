# Industrial-IOT-Gateway 运行手册

## 1. 适用范围

本手册适用于当前 Linux x86_64 软件候选版。树莓派 ARM64 与真实 USB-RS485 需要按同样步骤
重新验证，不能直接继承 WSL2 结论。

## 2. 构建与安装暂存

```bash
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGATEWAY_BUILD_TESTS=ON \
  -DGATEWAY_BUILD_PTY_SLAVE=ON \
  -DGATEWAY_ENABLE_MQTT=ON \
  -DGATEWAY_ENABLE_WARNINGS_AS_ERRORS=ON
cmake --build build/release -j2
ctest --test-dir build/release --output-on-failure

rm -rf /tmp/industrial_iot_gateway-stage
DESTDIR=/tmp/industrial_iot_gateway-stage cmake --install build/release
```

检查暂存树包含：

- `usr/local/bin/gateway_app`；
- `usr/local/lib/systemd/system/industrial_iot_gateway.service`；
- `usr/local/share/industrial_iot_gateway/systemd/gateway.env.example`。

## 3. 主机准备

```bash
sudo useradd --system --home /var/lib/industrial_iot_gateway \
  --shell /usr/sbin/nologin iot-gw
sudo usermod -aG dialout iot-gw
sudo install -d -o root -g iot-gw -m 0750 /etc/industrial_iot_gateway
sudo install -o root -g root -m 0755 gateway_app /usr/local/bin/gateway_app
sudo install -o root -g root -m 0644 \
  packaging/systemd/industrial_iot_gateway.service \
  /etc/systemd/system/industrial_iot_gateway.service
sudo install -o root -g iot-gw -m 0640 \
  config/examples/register_map.yaml \
  /etc/industrial_iot_gateway/register_map.yaml
```

优先使用 `/dev/serial/by-id/...`，不要把会变化的 `/dev/ttyUSB0` 当作长期设备名。

## 4. 环境配置

```bash
sudo install -o root -g iot-gw -m 0640 \
  packaging/systemd/gateway.env.example \
  /etc/industrial_iot_gateway/gateway.env
sudoedit /etc/industrial_iot_gateway/gateway.env
```

无 MQTT：

```text
GATEWAY_SERIAL_DEVICE=/dev/serial/by-id/REPLACE_ME
GATEWAY_REGISTER_MAP=/etc/industrial_iot_gateway/register_map.yaml
GATEWAY_MQTT_ARGS=
```

本地匿名 broker 验收：

```text
GATEWAY_MQTT_ARGS=--mqtt-broker-uri tcp://127.0.0.1:1883 --mqtt-client-id iiotgwLab01 --gateway-id lab_gateway_01
```

client ID 只用不超过 23 字符的字母数字；gateway ID 可含下划线。不要把生产密码写进公开模板。

## 5. 静态验证与启停

```bash
sudo systemd-analyze verify /etc/systemd/system/industrial_iot_gateway.service
sudo systemctl daemon-reload
sudo systemctl enable --now industrial_iot_gateway.service
systemctl status industrial_iot_gateway.service --no-pager
journalctl -u industrial_iot_gateway.service -n 100 --no-pager
```

修改环境或 unit 后必须 `daemon-reload`，再重启服务：

```bash
sudo systemctl restart industrial_iot_gateway.service
```

正常停止：

```bash
sudo systemctl stop industrial_iot_gateway.service
systemctl show industrial_iot_gateway.service \
  -p ActiveState -p SubState -p Result -p ExecMainStatus -p NRestarts
```

应看到 `gateway_summary`、`stopped=true`、退出 0；停止预算为 5 秒。

## 6. 日常观测

```bash
journalctl -u industrial_iot_gateway.service --since today --no-pager
journalctl -u industrial_iot_gateway.service --grep=gateway_ready --no-pager
journalctl -u industrial_iot_gateway.service --grep=mqtt_connect_failed --no-pager
systemctl show industrial_iot_gateway.service \
  -p MainPID -p NRestarts -p MemoryCurrent -p CPUUsageNSec
```

关注 `request_completed` 的结果分类、`mqtt_connect_failed`/reconnect、队列高水位、
`late_response_discarded` 和最终 `gateway_summary`。服务 active 但长期无成功请求仍需排查。

## 7. 分层排障

1. unit 无法加载：先运行 `systemd-analyze verify`，检查绝对路径和环境文件。
2. 退出 4：寄存器表缺失或内容错误；修正配置，不应依赖自动重启。
3. 服务 active 但 `serial_io_transient`：检查 by-id 路径、`dialout`、A/B/GND、波特率、
   校验位、从站地址和终端电阻。
4. `mqtt_connect_failed`：检查 broker 地址、监听端口和凭据；串口采集应继续。
5. 反复重启：查看 `NRestarts`、主进程退出码和 signal；不要先改成 `Restart=always`。
6. 停止超时：检查 MQTT drain、线程 join、串口 fd 所有权和 journal 最后的结构化事件。

## 8. 升级与回退

升级前保存当前二进制、unit、环境文件和寄存器表的 SHA-256；先在暂存目录执行
`cmake --install`，再停止服务、替换文件、`daemon-reload` 和启动。若新版本失败，恢复上一
组已知哈希的文件并再次重载。环境文件可能含生产信息，不得复制到公开 evidence。

## 9. 证据与权限边界

原始长稳、journal 和 RC 文件存于已忽略的 `artifacts/soak/`、`artifacts/systemd/`、
`artifacts/release/`。正式报告只引用去敏摘要。未经项目所有者单独授权，不执行
`git add`、`git commit` 或 `git push`。
