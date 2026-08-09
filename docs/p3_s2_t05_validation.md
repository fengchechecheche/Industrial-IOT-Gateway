# P3-S2-T05 PTY 从站模拟器与端到端通信验收记录

## 1. 验收结论

- 任务：`P3-S2-T05`（PTY 从站模拟器和基础端到端通信测试）。
- 执行日期：2026-08-09。
- 起始基线：`b74bd2468f3769668ebd69bc5ed8a97453be8c46`。
- WSL 开发验收：**通过**。
- 干净提交 Release 复验：**待用户批准提交后执行**。

本任务实现了可选构建的 `pty_slave` 工具。测试进程通过 `fork/exec` 启动真实模拟器
进程，模拟器使用 `openpty()` 创建 PTY 成对链路、用 T04 请求模式 parser 接收任意
`read()` 字节片段、依据冻结寄存器表执行请求，再使用 T03 codec 编码响应。测试端从
真实 PTY slave fd 发送请求并使用 T04 响应模式 parser 验证结果。

## 2. 实现范围

已实现：

- `GATEWAY_BUILD_PTY_SLAVE=ON` 时探测 `<pty.h>`、`libutil` 和 yaml-cpp；
- `pty_slave_support` 静态库和 `build/<config>/bin/pty_slave` 可执行程序；
- 从冻结 `register_map.yaml` 加载 slave 1、2、3 的 holding/input 地址空间；
- 独立运行场景文件提供确定性的初始寄存器值和故障默认值；
- `0x03`、`0x04`、`0x06` 正常交互，`0x06` 写入权限、范围和回显；
- 非法地址使用异常码 `0x02`，非法写值使用异常码 `0x03`；
- normal、exception、delay、silent、bad-crc、truncated 六类脚本化响应；
- `poll()` 非阻塞读写、部分读写、`EINTR/EAGAIN/EIO`、SIGINT/SIGTERM 和 fd RAII；
- 子进程就绪握手、退出状态、超时、PTY HUP 尾部数据排空和僵尸进程清理；
- `unit`、`integration` 和 `pty` 三类 CTest 标签。

不在本任务范围：S3 多从站轮询调度、response timeout 重试/退避、offline/recovery、
队列压力、MQTT、8 小时长稳、真实 UART/USB-RS485 或 RS485 电气层。

## 3. 配置和依赖

- 新增 yaml-cpp 0.8.0，用于读取 YAML，不使用外部 Modbus 协议库；
- PTY 使用 glibc 2.39 的 `openpty/libutil`，未安装 `socat`；
- 冻结源文件与仓库副本 SHA256 均为
  `b7410b4e1399007d7744714fdbf1f4e5ae7a51d81651057d95641c97327dc323`；
- 场景配置 SHA256 为
  `5a4edd6a8eb91f2dea78c4672c21aac1c4ea2954b37ba221b89d25dbe108c605`。

PTY 采用 raw 8-bit 用户态字节通道并保存 `B19200`。Linux PTY 不模拟 parity、停止位
或 RS485 电气时序；尝试对 PTY 强制 `PARENB` 会返回 `EINVAL`。因此项目默认配置仍是
`19200 8E1`，但本任务只验证完整 RTU 字节流和软件时间边界，不把 PTY termios 标志
表述为 8E1 硬件证据。

## 4. 测试先行证据

先加入 `RegisterBank`、`RuntimeConfiguration`、YAML loader 接口、CMake 接线和三项配置
测试，但不加入实现。原有代码和新测试均编译成功，最终在链接阶段只因 T05 接口未定义
而失败，包括：

```text
undefined reference to `industrial_iot_gateway::pty_slave::load_runtime_configuration(...)'
undefined reference to `industrial_iot_gateway::pty_slave::RegisterBank::read(...) const'
undefined reference to `industrial_iot_gateway::pty_slave::RegisterBank::write(...)'
collect2: error: ld returned 1 exit status
```

加入最小实现后，配置测试通过；真实 PTY 测试首先暴露了 PTY parity 的 `EINVAL` 边界，
随后又暴露了关闭 master 前未排空最后一帧的问题。实现改为 raw 字节通道，并在达到
`--max-requests` 后执行 `tcdrain()` 和 20 ms 排空窗口。最终所有场景稳定通过。

## 5. 端到端场景矩阵

| 场景 | 注入行为 | 观察结果 |
|---|---|---|
| normal | 正常编码 | `0x03/0x04/0x06`、写回显和写后读回均通过 |
| exception | 合法异常 ADU | 响应 parser 保留异常码 `0x04` |
| delay | 延迟 40 ms | 30 ms 之前不满足完成条件，随后收到合法响应 |
| silent | 不发送响应 | 主站测试端 120 ms 窗口内无字节，无忙循环 |
| bad-crc | 翻转 CRC 低字节一位 | 响应 parser 输出 `crc_mismatch` |
| truncated | 删除末尾两个字节 | `t3.5` 后输出 `frame_too_short` |

验收协议 F04 使用名称 `TRUNCATED_FRAME`，冻结协议和现有 parser 使用
`FRAME_TOO_SHORT`/`INTER_CHARACTER_TIMEOUT`。本任务将“截断响应在 t3.5 后”明确映射为
`frame_too_short`，没有增加语义重复的枚举。

## 6. 交付文件

| 文件 | 用途 | SHA256 |
|---|---|---|
| `tools/pty_slave/pty_slave_support.hpp` | 配置、寄存器和 PTY server 接口 | `5b5fa9c2ccdde82955b3915f9d1d7e96285f02929a4977fa08c386d33661e615` |
| `tools/pty_slave/pty_slave_support.cpp` | YAML、寄存器、故障注入、openpty 和进程循环 | `164cfd2026bc6082699a9499d9f1c987f23f702e96826323892c4ba8049d7c08` |
| `tools/pty_slave/main.cpp` | 独立工具入口 | `bd126473c417ae50792a5d0216f9762bb5d05cf9b61cd7cec79712b5f97d7678` |
| `tests/unit/pty_slave_config_test.cpp` | 三从站加载、地址空间和写限制测试 | `061c91acf0757526ebf79d87c4cbd277ef84417b5b4ea3b084e4b57c5c231b7a` |
| `tests/integration/pty_slave_integration_test.cpp` | 真实 PTY 六场景进程级测试 | `ef2ca7da4aa46f5ef6b2d789041cc87f0eaf55388e36535b1bee3e276a616c9e` |

另修改根 `CMakeLists.txt`、`tests/CMakeLists.txt`，并新增两份公开示例配置。

## 7. 质量门

最终内容均在 `GATEWAY_BUILD_PTY_SLAVE=ON` 条件下执行：

| 配置 | 构建 | CTest | 结果边界 |
|---|---|---:|---|
| Debug + 警告视为错误 | 通过 | 44/44 | 37 unit、6 integration、1 PTY 聚合门 |
| Debug + ASan/UBSan + 警告视为错误 | 通过 | 44/44 | 无 sanitizer 报告 |
| Release + 警告视为错误 | 通过 | 44/44 | 未提交工作区开发证据 |
| Debug + clang-tidy + 警告视为错误 | 通过 | 44/44 | clean-first 全量构建零告警 |

44 项中有 43 个独立 GoogleTest 用例，另有一个带 `pty` 标签的聚合门，它重新运行 6 个
真实 PTY 场景，确保 `ctest -L pty` 可独立作为准入命令。`clang-format --dry-run
--Werror` 和 `git diff --check` 均通过。

## 8. 后续边界

T05 已向 S3 移交可执行模拟器、三个从站的冻结配置加载能力和六类故障开关。S3 仍需实现
生产串口 transport、单在途请求调度、多从站轮询、response timeout、最多三次尝试、
有限退避、offline/recovery、freshness 和有界关闭。没有这些证据时，不得宣称完整 G2/G3
已经通过；PTY 结果也不得作为真实 RS485 电气验证结果。
