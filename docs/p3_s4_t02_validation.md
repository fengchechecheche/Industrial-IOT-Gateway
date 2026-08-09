# P3-S4-T02 验收报告

> 状态：已审核固化。  
> 验证日期：2026-08-09。  
> 验证仓库：`/home/iot-gw/projects/industrial_iot_gateway`。  
> 权限边界：未检查 Git 工作区状态，未执行 `git add`、`git commit` 或 `git push`。

## 1. 验收结论

P3-S4-T02 的软件工程目标已经实现并通过当前环境的质量门：MQTT 异步发布、有界缓存、
fresh 合并、关键容量预留、断线重连、状态快照、指标和有界关闭均有自动化证据；broker
故障期间 PTY 串口轮询继续。

该结论只覆盖 Ubuntu WSL2 上的回环匿名 Mosquitto 软件链路，不扩展为 TLS、生产认证、远程
网络、真实 RS485 或 STM32 硬件结论。教程和本报告已经用户审核通过，并同步到独立仓库
`docs/learning/` 与 `docs/` 正式维护。

## 2. 主要工程产物

| 类别 | 产物 |
|---|---|
| 构建 | `CMakeLists.txt`、`cmake/MqttDependencies.cmake`、`GATEWAY_ENABLE_MQTT` |
| 抽象发布 | `publish/publish_sink.hpp`、`publish/jsonl_publish_sink.hpp/.cpp` |
| MQTT | `mqtt_config.hpp`、`mqtt_publication.hpp`、`payload_serializer.*`、`reconnect_policy.*`、`mqtt_publish_sink.*` |
| 运行时 | `GatewayRuntime` 注入 PublishSink、质量快照、设备状态和 2000 ms drain |
| 应用 | MQTT 三参数组、运行期 JSONL 回退、gateway summary 指标 |
| 测试 | MQTT policy/payload/queue 单元测试、Mosquitto 重连测试、MQTT+PTY 持续轮询测试 |
| 教学 | `p3_s4_t02_异步mqtt发布与有界缓存.md` 已审核固化版本 |

## 3. 依赖与环境证据

| 依赖 | 验证版本 | 用途 |
|---|---:|---|
| Eclipse Paho MQTT C++ | 1.2.0-2 | `mqtt::async_client` C++ 接口 |
| Eclipse Paho MQTT C | 1.3.13-1build2 | 异步网络后端 |
| nlohmann/json | 3.11.3-1 | payload 序列化与测试解析 |
| Mosquitto / clients | 2.0.18-1build3 | 只用于本地 broker 集成测试 |

首轮验收使用精确版本 deb 临时解包完成。随后项目所有者执行了固定版本系统安装命令，
`dpkg-query` 确认五个软件包均已安装且版本完全匹配。本次系统依赖复验没有设置临时
`CMAKE_PREFIX_PATH` 或 `LD_LIBRARY_PATH`；CMake 直接从 `/usr/include` 和
`/usr/lib/x86_64-linux-gnu` 发现依赖，MQTT ON 全目标构建通过，完整 CTest 为 168/168。
`ldd` 同时确认 `gateway_app` 从系统库目录加载 Paho，未发现缺失动态库。复验使用的
`/tmp/industrial_iot_gateway_t02_system_debug` 构建目录已在记录结果后删除，本机 MQTT ON
构建所需的依赖准备项现已关闭。

## 4. 验收矩阵

| ID | 要求 | 证据 | 结果 |
|---|---|---|---|
| A01 | MQTT 可独立启停 | MQTT OFF 158/158；MQTT ON 168/168 | PASS |
| A02 | `gateway_core` 不公开 Paho 类型 | `PublishSink` 抽象；Paho 只在 `gateway_mqtt` | PASS |
| A03 | 冻结 JSON、QoS/retain 映射 | payload serializer 测试覆盖 fresh/invalid/stale/offline-no-history/status/health | PASS |
| A04 | 普通 fresh 按 topic 合并 | `CoalescesFreshTelemetryBeforeNetworkThreadStarts` | PASS |
| A05 | 高水位后保留关键容量 | `PreservesCriticalCapacityAfterNormalLimit` | PASS |
| A06 | 断线、重连与恢复快照 | `MqttBrokerIntegrationTest...` | PASS |
| A07 | 旧 fresh 超期不得继续 fresh 发布 | 同一 broker 集成测试检查 `expired_fresh_dropped` | PASS |
| A08 | stale/offline 沿用最后有效值 | PTY 记录型发布器消息级断言 | PASS |
| A09 | broker 故障不停止串口 | `MqttPtyRuntimeIntegrationTest...` 中成功请求继续增长至少 10 | PASS |
| A10 | SIGTERM/关闭有界排空 | 2000 ms sink deadline；broker 测试实测低于 2500 ms | PASS |
| A11 | 连接、发布、队列和关闭指标 | `PublishSinkStatistics` 与 `gateway_summary` | PASS |
| A12 | 线程生命周期无竞态 | TSan 168/168；disconnect token 生命周期修复后复测 | PASS |
| A13 | 不开放 MQTT 下行写寄存器 | serializer/sink 只有上行消息路径 | PASS |

## 5. 测试先行证据

1. 首个 reconnect policy 测试在头文件尚不存在时编译失败，随后实现最小策略后转绿。
2. stale/offline PTY 消息级测试首次运行时两项均失败，因为运行时只发布质量转换、没有发布
   历史值遥测；补充最后有效样本缓存后转绿。
3. PTY+MQTT 重连测试暴露“Paho 连接 token 在等待边界后完成、指标未记账”的竞态窗口；
   增加 worker 侧连接确认后转绿。
4. TSan 首轮 164/168，定位到测试记录器统计未加锁和 Paho disconnect token 过早释放；
   修复后精确复测 3/3，再全量复测 168/168。

## 6. 质量门结果

所有构建目录都位于 `/tmp`，没有在仓库内生成 `build-*`；记录结果后已清理这些临时目录：

| 质量门 | 配置 | 结果 |
|---|---|---|
| Debug 回归 | MQTT OFF + PTY | PASS，158/158 |
| Debug 集成 | MQTT ON + PTY + Mosquitto | PASS，168/168 |
| ASan/UBSan | MQTT ON + PTY + Mosquitto | PASS，168/168 |
| Release | MQTT ON + PTY + Mosquitto | PASS，168/168 |
| clang-tidy | MQTT ON 全目标 | PASS，构建完成；新增 MQTT 库源码诊断已清理 |
| TSan | MQTT ON + WSL2 `setarch -R` | PASS，168/168 |
| clang-format | clang-format 18 `--dry-run --Werror` | PASS |

固定版本依赖写入系统包数据库后，又执行了一次不使用临时依赖路径的 Debug、MQTT ON、
Mosquitto 和 PTY 干净复验；配置、全目标构建和 CTest 均通过，CTest 结果为 168/168。

## 7. 已验证行为

- MQTT 3.1.1、clean session、QoS 1；
- telemetry 不使用 MQTT RETAIN；device status、gateway health、LWT 使用 RETAIN；
- fresh topic 合并和关键容量预留；
- 1/2/4/8/16/30 秒退避以及 ±20% 抖动；
- broker 离线时串口轮询继续；
- 重连后 health/status 快照优先，过期 fresh 被丢弃；
- stale/offline 历史值和 offline-no-history 字段规则；
- 最多 2000 ms MQTT 排空，总关闭预算不扩张；
- MQTT OFF 保持原 JSONL/PTY 链路。

## 8. 未验证与后续移交

- 未安装或验证 TLS、用户名密码、ACL、证书和生产 broker；
- 未验证跨主机网络、网络分区、DNS 故障或大规模网关惊群；
- 当前缓存位于内存，未验证进程崩溃后的持久恢复；
- T03 才实现 F01–F15 fault runner、慢消费者、统一 `summary.json/failures.json`；
- 未执行真实 USB-RS485/STM32 硬件台架；
- 未验证 systemd、ARM64 或生产部署。

建议审核通过后：

1. 将教程同步为 `docs/learning/p3_s4_t02_异步mqtt发布与有界缓存.md`；
2. 将本报告同步为 `docs/p3_s4_t02_validation.md`；
3. 更新 `docs/learning/README.md` 的 T02 状态；
4. 由项目所有者安装固定版本 MQTT 依赖；
5. 再单独制定并审核 P3-S4-T03 故障注入 runner 详细计划。
