# P3-S3-T04 有界队列、显式请求公平性、关闭协议与结构化日志验收记录

## 1. 验收结论

- 任务：`P3-S3-T04`（有界队列、显式请求公平性、关闭协议与结构化日志）。
- 执行日期：2026-08-09。
- 环境：`Ubuntu-24.04-Gateway`，x86_64，Linux 文件系统仓库。
- WSL 开发验收：**通过**。
- ThreadSanitizer 运行验收：**通过**（WSL2 进程级地址布局兼容入口，112/112）。
- 提交后固定版本 Release 复验：**未执行**。该步骤需要用户另行授权 Git 操作并形成固定提交。

本任务在 T03 的调度、重试和新鲜度能力之上，实现了固定容量的跨线程队列、三类业务队列的
差异化满载策略、显式请求与周期轮询之间的确定性公平规则、同步信号等待和幂等关闭状态机，
以及线程安全的 JSON Lines 结构化日志基础设施。

本结论不表示 scheduler、transport、codec/parser、MQTT 和三个 PTY 已经组成可运行的完整网关
主循环；该集成仍属于 `P3-S3-T05`。真实 MQTT broker、UART/USB-RS485 和 RS485 电气验证也不在
本任务范围内。

## 2. 有界队列公共语义

`BoundedQueue<T>` 提供固定容量、先进先出、有限等待和显式关闭语义：

- 构造时拒绝零容量，以及为零或不小于容量的高水位；
- `try_push()` 在队满时立即返回可观察的 `full`，不会无界增长；
- `push_until()` 和 `wait_pop_until()` 仅等待到调用方给定的 `steady_clock` 截止点；
- `close()` 幂等，唤醒所有阻塞生产者和消费者，关闭后拒绝新写入；
- 关闭前已经接收的元素仍可按 FIFO 顺序排空；
- 统计包含接收、弹出、首次越过高水位、满载、合并、关闭拒绝和等待超时次数；
- 队列互斥区内不执行串口阻塞 I/O、MQTT 网络调用或外部业务回调。

并发测试以多个生产者和一个消费者验证固定总数的消息无丢失、无重复，并验证关闭可以唤醒
阻塞消费者。

## 3. 三类队列与满载策略

| 队列 | 默认容量 | 高水位 | 允许合并 | 严禁静默覆盖 |
|---|---:|---:|---|---|
| 请求队列 | 256 | 205 | 相同 `poll_job_id` 的待处理周期轮询，仅保留最新项 | 显式写请求 |
| 测量队列 | 512 | 410 | 相同从站、功能码和起始地址的普通原始测量 | 请求结果、质量转换 |
| 发布队列 | 1024 | 820 | 相同 topic 的 fresh 普通遥测 | 质量转换、写操作审计 |

所有入队操作都返回 `accepted`、`coalesced`、`full`、`closed` 或 `timed_out` 等显式结果。关键
事件在容量耗尽时返回 `full`，由上层决定退避、告警或关闭，不会把丢失伪装为成功。

## 4. 显式请求公平性

请求队列分别维护显式请求和周期轮询项，并冻结以下确定性规则：

1. 显式请求优先，但最多连续派发 8 个；
2. 达到 8 个后，只要存在到期且已满足 T03 attempt 资格时间的轮询项，就必须先派发一个轮询；
3. 尚未到期的轮询项不会为了形式公平而让总线空闲，仍可继续派发显式请求；
4. 周期轮询按到期资格和稳定入队顺序选择；
5. 相同轮询任务仅合并待处理副本，已经派发或正在重试的业务请求不被覆盖；
6. `close()` 后拒绝新请求，但允许已经接收的请求被上层排空或取消。

单元测试覆盖默认参数、非法公平上限、周期轮询合并、显式写不覆盖、精确 8:1 边界、未来轮询
不造成空转，以及关闭后的排空行为。

## 5. 关闭协议与信号处理

`ShutdownCoordinator` 默认总优雅关闭预算为 5000 ms，MQTT 排空预算为 2000 ms。关闭阶段固定为：

```text
running
  -> stop_accepting
  -> cancel_queued_and_retry_wait
  -> wake_serial_io
  -> stop_polling
  -> drain_or_expire_measurements
  -> drain_mqtt_with_limit
  -> close_mqtt
  -> serial_io_owner_closes_fd
  -> join_all_threads
  -> stopped
```

- 首次停止请求冻结原因和起始时刻；重复请求只累计计数，不会重入关闭流程；
- 非法跳步和时钟回退返回明确错误，不改变当前阶段；
- 在精确 5000 ms 边界判定总关闭预算耗尽；
- 串口文件描述符只允许串口 I/O 所有者关闭一次；
- SIGINT/SIGTERM 先在工作线程继承前阻塞，再由专用线程使用 `sigwait()` 同步等待，不在异步
  signal handler 中调用互斥锁、条件变量、日志或其他非异步信号安全代码。

集成测试派生子进程并发送真实 SIGTERM，验证信号等待线程触发关闭、阻塞队列被唤醒、线程在
5 秒预算内完成 join、串口所有者恰好关闭一次且子进程正常退出。

## 6. JSON Lines 结构化日志

`StructuredLogEvent` 和 `format_jsonl()` 为请求状态转换、队列压力和关闭流程提供统一单行 JSON：

- 支持时间戳、级别、事件名、组件、`request_id`、`slave_id`、功能码、attempt、状态、错误类别、
  队列名/深度/容量、关闭阶段和附加消息；
- 缺失的可选字段不输出，避免用虚假默认值污染事件；
- 对引号、反斜杠和控制字符进行 JSON 转义；
- 每个事件恰好写成一行；
- `JsonlWriter` 以互斥锁保护完整行写入，多个线程不会交叉拼接日志；
- 输出失败以返回值和计数器显式暴露，不在失败路径递归记录日志。

T04 交付的是可复用的日志事件和写入基础设施；具体网关主循环在 T05 集成时负责在真实请求、
队列和关闭转换点构造并发送事件。

## 7. 测试先行证据

先加入公共接口、测试源文件和测试目标，不加入实现源文件。5 个单元测试源文件成功编译，链接
阶段随后按预期出现 `BoundedQueue`、`RequestQueue`、`ShutdownCoordinator`、`format_jsonl()` 和
`JsonlWriter` 的 undefined reference。加入最小实现后，T04 定向单元测试最终 31/31 通过，真实
SIGTERM 集成测试 1/1 通过。

新增测试覆盖：

- 7 个通用有界队列测试；
- 7 个请求队列和公平性测试；
- 6 个测量/发布队列策略测试；
- 6 个关闭协调器测试；
- 5 个结构化日志测试；
- 1 个同步 SIGTERM、队列唤醒、线程 join 和资源单次关闭集成测试。

## 8. 质量门

四套基础配置均在最终代码上执行 clean-first 或最终全量测试；另对全部非 PTY 测试执行 TSan：

| 配置 | 构建 | CTest | 其他结果 |
|---|---|---:|---|
| Debug | 通过 | 127/127 | 无编译警告 |
| Debug + ASan/UBSan | 通过 | 127/127 | 无 sanitizer 报告 |
| Release | 通过 | 127/127 | 完整测试通过 |
| Debug + clang-tidy | 通过 | 127/127 | 最终全量构建零告警 |
| Debug + TSan | 通过 | 112/112 | 无数据竞争报告；PTY 工具未在此配置中构建 |

最终标签统计：

- `unit`：115/115，其中新增 T04 用例 31 个；
- `integration`：10/10，其中新增 T04 用例 1 个；
- `pty`：2/2 聚合门。

16 个 T04 C++ 文件均通过 `clang-format-18 --dry-run --Werror`。clang-tidy 首轮指出无效移动、
`std::optional` 访问证明和测试容器预留问题；修正后最终全量构建零告警。

初次直接运行 ThreadSanitizer 二进制时，程序在进入 GoogleTest 之前由 WSL2 运行时终止：

```text
FATAL: ThreadSanitizer: unexpected memory mapping 0x...-0x...
```

定向试验确认 `setarch x86_64 -R` 可以仅为当前测试进程选择兼容地址布局，不需要 `sudo`，也不
修改系统全局 ASLR 参数。在此基础上新增 `TsanTestRuntime.cmake`：

- 仅当 `GATEWAY_ENABLE_TSAN=ON`、`GATEWAY_ENABLE_WSL_TSAN_WORKAROUND=ON` 且内核 release 匹配
  WSL2 时启用；
- 自动查找 `setarch`，缺失时在配置阶段明确失败；
- 通过 GoogleTest 目标的 `CROSSCOMPILING_EMULATOR` 属性，使构建阶段测试发现和 CTest 执行都
  自动使用 `/usr/bin/setarch x86_64 -R`；
- 原生 Linux、普通 Debug/Release、ASan/UBSan 和 clang-tidy 配置不添加该执行器；
- 兼容开关默认开启，但只有同时启用 TSan 且检测到 WSL2 时才产生行为变化。

清除先前手工设置的 PRE_TEST 缓存后，以默认 GoogleTest POST_BUILD 发现模式 clean-first 构建
39/39 步骤通过；随后直接执行普通 `ctest --test-dir build-tsan --output-on-failure`，无需手工添加
`setarch`，全部 112/112 个已注册测试通过，其中 `unit` 111/111、`integration` 1/1，且无 TSan
数据竞争报告。普通 Debug 配置随后复验 127/127，通过结果未受影响。

该结论证明当前 WSL2 开发环境中的 TSan 质量门已经可重复运行；原生 Linux/CI 仍应保持不关闭
ASLR 的普通 TSan 入口，以同时覆盖不同运行环境。

依据仓库根 `AGENTS.md` 权限红线，本任务未执行 `git status`、用于检查工作区的 `git diff`、
暂存、提交或推送。文件正确性通过定向读取、编译、测试、静态检查、格式检查和 SHA256 固化
验证。

## 9. 交付文件

| 文件 | 用途 | SHA256 |
|---|---|---|
| `include/industrial_iot_gateway/concurrency/bounded_queue.hpp` | 通用固定容量队列、关闭语义和统计 | `d93733f638b2fa14f7b6b8a79b4d3889b545e0d03e80f966a0178f825b221509` |
| `include/industrial_iot_gateway/pipeline/pipeline_messages.hpp` | 请求、测量、发布和质量事件消息类型 | `b06f9fee34c1145e94391c1c283a55475a4010b227f8f220de7a5ceb70cc19dd` |
| `include/industrial_iot_gateway/pipeline/request_queue.hpp` | 请求队列和公平性公共接口 | `6cd47bb45151d9fb097c2a78d6b7896e8a31bf02defc3c95498668e138dce97e` |
| `src/pipeline/request_queue.cpp` | 显式请求优先、8:1 公平和轮询合并实现 | `99e503daad8d105d2218d204cd432510fdf2812e0e964926f20bec2780768a49` |
| `include/industrial_iot_gateway/pipeline/event_pipeline.hpp` | 测量/发布队列公共接口 | `893a811f09ee809a5180a7e010e59fff38226dd7cd018ab9821eaf5dbb60e37e` |
| `src/pipeline/event_pipeline.cpp` | 三类消息的差异化合并与满载策略 | `0583a0fba5eb2bc2513c0abb93008063d00f37589069bff11aaf131b867cacc8` |
| `include/industrial_iot_gateway/lifecycle/shutdown_coordinator.hpp` | 关闭状态机、预算和同步信号接口 | `7b9a49c90b586813fc21b1a7da005799c1c6ff4afd898164ea8e110c7aa25147` |
| `src/lifecycle/shutdown_coordinator.cpp` | 幂等关闭、阶段校验和 `sigwait()` 实现 | `2ceb9fcb646e43bd5a092148dd0e48ff9afcd97065e6317506089e8d808b9b80` |
| `include/industrial_iot_gateway/observability/structured_log.hpp` | JSONL 事件和线程安全写入接口 | `2e2259961ab49c555010bb370b37e24e8b0baa17186ce190911c8e46d82127d8` |
| `src/observability/structured_log.cpp` | JSON 转义、单行序列化和写入实现 | `fe1fd73dfef1841b9333d0dc6213720afad1663f15290053d0826cf746c0b6c8` |
| `tests/unit/bounded_queue_test.cpp` | 容量、FIFO、超时、关闭和并发测试 | `d8e4d52ef753071f3fc78a32f92fa81fa025c894cff0b4450c29643beb30d898` |
| `tests/unit/request_queue_test.cpp` | 请求合并、满载和 8:1 公平测试 | `71d854a5188ac76cce5c774e6d1837a43afc4a935d63fad8eb003981af1316da` |
| `tests/unit/event_pipeline_test.cpp` | 测量与发布队列策略测试 | `aa22146b62a9988cc8b03822a17d8cd4f933db8dc09d50ec2a480417c091243b` |
| `tests/unit/shutdown_coordinator_test.cpp` | 关闭状态、边界和幂等测试 | `103fff38d28c27b888f62a041a1c28754e2c4590c46474020b2f5d07e7af3327` |
| `tests/unit/structured_log_test.cpp` | JSONL 字段、转义、并发和失败测试 | `a6a1e491d5151b92868d90ff731ab6be0a926294f0d7183ad8df6c0761fa525b` |
| `tests/integration/shutdown_signal_integration_test.cpp` | 真实 SIGTERM 与关闭集成测试 | `9db5fa3ac2e657737803bf6a010c1982d159d24b059a61cf935c8039cf54caf1` |
| `CMakeLists.txt` | TSan WSL2 兼容开关和运行时模块接入 | `ec08cf875e6dcb5b31e54138a60cfa6582103629d458511b699c2bba2efe492f` |
| `cmake/TsanTestRuntime.cmake` | WSL2 检测和进程级 `setarch` 测试执行器 | `2783229115bd1a9b182ad5e4d19b5d98adcfedcf18afe6cef8eccb64564089e1` |
| `tests/CMakeLists.txt` | 将全部 GoogleTest 目标接入条件式执行器 | `610b7bad311821b74cb4ad2ccd9a8ccfae6db2f05dc5bca1847b841f3b596e69` |

根 `CMakeLists.txt` 和 `tests/CMakeLists.txt` 同时承担原有 T04 源文件接入、线程库链接、测试注册
以及上述条件式 TSan 执行器接入。

## 10. 后续边界

T04 向后续任务移交有界消息通道、确定性显式请求公平性、可验证的关闭协议和 JSONL 日志能力。
下一步应实施 `P3-S3-T05`，把 scheduler、transport、codec/parser、可靠性、质量跟踪、T04 队列和
三个 PTY 连接为真实的端到端网关主循环，并验证正常轮询、写请求、异常、超时、重试、离线恢复
和受控关闭。MQTT broker 集成若仍按项目路线位于后续阶段，则不得在 T05 验收中提前宣称完成。
