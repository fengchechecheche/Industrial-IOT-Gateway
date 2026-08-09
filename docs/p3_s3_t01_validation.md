# P3-S3-T01 termios 与非阻塞串口封装验收记录

## 1. 验收结论

- 任务：`P3-S3-T01`（`termios` 与非阻塞串口封装）。
- 执行日期：2026-08-09。
- 环境：`Ubuntu-24.04-Gateway`，x86_64，Linux 文件系统仓库。
- WSL 开发验收：**通过**。
- 提交后固定版本 Release 复验：**未执行**。该步骤需要用户另行授权 Git 操作并形成固定提交。

本任务实现了 Linux POSIX 串口 transport，提供显式参数配置、move-only RAII、非阻塞
读写、基于 `poll()` 和 `steady_clock` deadline 的事件等待、结构化错误分类、断连识别和
单次显式重新打开。实现未加入 scheduler、自动重试/退避、Modbus 请求编排、MQTT、
`TIOCSRS485` 或真实 RS485 电气控制。

## 2. 配置合同

- 默认配置为冻结协议规定的 `19200 8E1`；
- 支持 `9600`、`19200`、`38400`、`115200`；
- 固定 8 数据位，支持 none/even/odd parity 和 1/2 stop bits；
- 使用 `O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC`；
- 使用 raw mode，关闭软件流控和可用的平台硬件流控；
- `VMIN=0`、`VTIME=0`，timeout 统一由 `poll()` deadline 决定；
- 打开或配置失败只返回一次错误，不在 transport 内循环重试。

PTY 不能证明 parity、stop bit 或 RS485 电气行为。真实 PTY 测试显式使用兼容的 `8N1`；
默认 `8E1` 和其他参数组合通过注入的 POSIX API 单元测试验证 flag/speed 映射。

## 3. 公共接口与错误模型

公开头文件提供：

- `SerialConfig`、`BaudRate`、`Parity`、`StopBits`；
- move-only `SerialPort`；
- `open()`、`reopen()`、`close()`、`is_open()`；
- `read_some()`、`write_some()`、`wait()`；
- `SerialErrorCategory`、`IoStatus`、`WaitStatus` 和对应结构化结果。

短读和短写返回实际字节数；`EINTR` 在 transport 内重试；`EAGAIN/EWOULDBLOCK` 返回
`would_block`；`poll()` 每次因 `EINTR` 重新进入时都根据原始 deadline 计算剩余时间。
`POLLHUP/POLLERR/POLLNVAL` 和确认的终端 I/O 错误映射为 `disconnected`。断连时 fd 由
唯一所有者关闭一次；`reopen()` 只进行一次显式打开，不包含后台循环或退避策略。

由于 noncanonical `VMIN=0/VTIME=0` 下零字节读取也可以表示当前无数据，`read()==0`
保守映射为 `would_block`；对端关闭由 `poll()` HUP/ERR 或后续 `EIO` 确认。

## 4. 测试先行证据

先加入公共接口、内部 POSIX 调用缝、14 个单元测试和 CMake 测试目标，但不加入
`serial_port.cpp` 实现。测试源成功编译，链接阶段因实现缺失而按预期失败，包括：

```text
undefined reference to `industrial_iot_gateway::transport::SerialPort::SerialPort()`
undefined reference to `industrial_iot_gateway::transport::SerialPort::open(...)`
undefined reference to `industrial_iot_gateway::transport::SerialPort::read_some(...)`
undefined reference to `industrial_iot_gateway::transport::SerialPort::write_some(...)`
undefined reference to `industrial_iot_gateway::transport::SerialPort::wait(...)`
undefined reference to `industrial_iot_gateway::transport::SerialPort::reopen(...)`
```

加入最小实现后，首次编译暴露私有头文件包含路径错误；修复为同目录私有 include，没有把
`src/` 扩展成 `gateway_core` 的公开 include 路径。随后 14/14 单元测试通过。

一次 move 所有权测试失败来自测试在 fake API 析构后读取悬空观察指针；改为由测试作用域
持有外部关闭计数器后通过，生产实现不需要为该测试缺陷改变语义。

## 5. 自动测试覆盖

单元测试覆盖：

- 冻结默认 `19200 8E1` 和四档波特率；
- raw、nonblocking、close-on-exec、parity、stop bits、`VMIN/VTIME`；
- 非法配置和打开失败单次返回；
- 配置失败关闭一次、move 后唯一关闭者；
- 短读、短写、`EINTR`、`EAGAIN/EWOULDBLOCK`；
- 有限 `poll()` timeout 和原 deadline 内的 `EINTR` 重试；
- HUP 断连、关闭一次和显式重新打开。

真实 PTY 集成测试覆盖：

- 双向原始字节流；
- 分片读取和无数据时 `would_block`；
- 关闭 PTY master 后识别断连；
- 同一 `SerialPort` 对象在新 PTY 上重新打开并恢复通信。

## 6. 质量门

所有配置均启用 `GATEWAY_BUILD_PTY_SLAVE=ON` 和项目代码警告视为错误：

| 配置 | clean-first 构建 | CTest | 其他结果 |
|---|---|---:|---|
| Debug | 通过 | 62/62 | 无编译警告 |
| Debug + ASan/UBSan | 通过 | 62/62 | 无 sanitizer 报告 |
| Release | 通过 | 62/62 | 完整测试通过 |
| Debug + clang-tidy | 通过 | 62/62 | 最终全量构建零告警 |

Release 独立标签门：

- `unit`：51/51；
- `integration`：9/9，其中新增串口 PTY 用例 3 个；
- `pty`：2/2 聚合门，其中 `serial_port_pty_suite` 内部执行 3 个真实 PTY 用例。

clang-tidy 首轮发现测试 fake 的可交换整数参数和 moved-from 读取两条告警；前者改用
`std::chrono::milliseconds` 强类型，后者删除非必要读取，最终 clean-first 全量构建零告警。
T01 五个新增 C++ 文件均通过 `clang-format-18 --dry-run --Werror`。

依据仓库根 `AGENTS.md` 权限红线，本任务未执行 `git status`、用于检查工作区的
`git diff`、`git diff --check`、暂存、提交或推送。文件正确性通过定向读取、编译、测试、
静态检查、格式检查和 SHA256 固化验证。

## 7. 交付文件

| 文件 | 用途 | SHA256 |
|---|---|---|
| `include/industrial_iot_gateway/transport/serial_port.hpp` | 公共串口 API | `72025f1992214fed2e7f182ff1b5f54caf3e10047c7e513e6c8eaf6435bb9480` |
| `src/transport/serial_port_detail.hpp` | 私有 POSIX 调用缝 | `36cf0a778618528e77f86c7352a94740b1dfba4dca700fc32a90bf6da575f6df` |
| `src/transport/serial_port.cpp` | termios、非阻塞 I/O 和 fd 生命周期 | `355a45aa8f6eb9b1297322a5efb78ad143d01d995dd3bc4adef27145b5c6453c` |
| `tests/unit/serial_port_test.cpp` | 确定性错误与边界测试 | `d0101351df2f025176c0ac940ef1af0210a6ac3286a1d7314f329fccc57f6ea9` |
| `tests/integration/serial_port_pty_test.cpp` | 真实 PTY 通信、断连和重开 | `fa91c1446420fcd6015bdd3acc0c19f0ce2a4970524ff1a7c0546fa2769d5619` |

另修改根 `CMakeLists.txt` 和 `tests/CMakeLists.txt`，把 transport 加入 `gateway_core`，并
注册单元、integration 和独立 PTY 聚合门。

## 8. 后续边界

T01 只向后续任务移交一个非阻塞、可等待、可显式重开的串口 transport。以下能力仍未
实现，不能由本记录推定完成：

- `P3-S3-T02` 单总线单在途请求和多从站轮询；
- `P3-S3-T03` response timeout、有限重试、退避、offline/recovery 和 freshness；
- `P3-S3-T04` 队列、线程关闭和结构化日志；
- `P3-S3-T05` 三从站软件网关集成矩阵；
- MQTT、真实 UART/USB-RS485 和 RS485 电气验证。
