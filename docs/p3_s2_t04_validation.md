# P3-S2-T04 增量流式组帧器验收记录

## 1. 验收结论

- 任务：`P3-S2-T04`（增量流式组帧器与 corpus seeds）。
- 执行日期：2026-08-09。
- 代码基线：`6c90878dfcbd2ea2e14f916ceec8932845b8cceb`。
- WSL 开发验收：**通过**。
- 干净提交 Release 复验：**待用户批准提交后执行**。

本任务实现了请求流和响应流双模式的无状态业务外 parser。它接收任意长度字节片段，使用固定 512 字节内部缓冲、调用方提供的固定事件数组和准确的 `bytes_consumed` 实现有界输入/输出；完整候选帧统一交给 T03 codec 执行 CRC、长度、功能码和请求上下文校验。

## 2. 实现范围

已实现：

- 请求模式：提取 `0x03`、`0x04`、`0x06` 固定 8 字节请求，供后续 PTY 从站复用；
- 响应模式：依据当前 `request_id` 和 `Request` 推导读取、写回显和异常响应长度；
- 整帧、逐字节、任意两段分片和多个粘连帧；
- 输出容量耗尽时停止消费并返回准确偏移；
- 噪声前缀、坏 CRC、错误从站、错误 `byte_count` 和超长候选后的有界重新同步；
- `t1.5` 字符间超时和 `t3.5` 帧边界；
- 已知/未知异常响应保留；
- 空请求上下文拒绝、最大 125 寄存器/255 字节响应和 4096 字节噪声输入；
- 7 个文本十六进制 corpus seeds。

不在本任务范围：真实串口读取、PTY、termios、response timeout、重试、调度、寄存器读写、MQTT，以及正式 libFuzzer/AFL++ campaign。

## 3. 有界性和接口决策

- 最大 ADU：256 字节；
- parser 内部缓冲：固定 512 字节；
- 输出：调用方提供 `ParserEvent[]` 和容量；
- 输出满：停止消费，设置 `output_full=true`，调用方从 `bytes_consumed` 继续；
- 时间：使用强类型 `MonotonicTimeUs`，避免在 64 位 Linux 上把 `size_t` 长度和 `uint64_t` 时间误传；
- 所有公共 parser 操作保持 `noexcept`；
- 无动态事件队列、无递归、无 `sleep()` 测试、无无限扫描。

响应模式只接受与当前请求匹配的从站、正常功能码或异常功能码。完全相同的连续 Modbus RTU 请求在线路上没有 transaction ID，parser 无法仅凭 ADU 区分旧请求的迟到响应和新请求响应；后续 scheduler 必须用单在途请求、deadline、输入清理和静默边界降低该风险，T04 不伪造协议不具备的区分能力。

## 4. 测试先行证据

先加入接口、测试、CMake 接线和 corpus，但不加入实现。原有目标及新 parser 测试均成功编译，最终只在链接阶段因 `RtuStreamParser` 构造、模式切换、`ingest`、时间推进和状态查询方法未定义而失败：

```text
undefined reference to `industrial_iot_gateway::protocol::RtuStreamParser::RtuStreamParser(...)'
undefined reference to `industrial_iot_gateway::protocol::RtuStreamParser::ingest(...)'
undefined reference to `industrial_iot_gateway::protocol::RtuStreamParser::on_time_advanced(...)'
collect2: error: ld returned 1 exit status
ninja: build stopped: subcommand failed.
```

加入最小实现后转为 28/28；补齐上下文、最大响应、调用间超时和超长候选后，最终为 33/33。

首次 clang-tidy 报告 `size` 与 `arrival_time_us` 在当前平台解析为相同底层类型、容易交换。接口引入 `MonotonicTimeUs` 强类型后，全量清理重建零警告。

错误 `byte_count` 后恢复测试表明：丢弃首字节后，坏帧残余可能偶然形成额外语法候选。parser 会有界报告该候选的 CRC 错误并继续扫描，测试因此要求“首个分类正确、最后恢复合法帧”，不错误要求噪声路径只能产生恰好一个错误事件。

## 5. 交付文件

| 文件 | 用途 | SHA256 |
|---|---|---|
| `include/industrial_iot_gateway/protocol/rtu_stream_parser.hpp` | parser 类型、事件、时间强类型和公开 API | `69da1b9e4dc4ec70b2abc72cdb9610d532f14f216669fae4fecc081a6283ce9f` |
| `src/protocol/rtu_stream_parser.cpp` | 增量缓存、候选长度、codec 门控和重新同步 | `8bbab99ee44efe498c442b577a585206b5557a1398c56befff04822d61f4f85d` |
| `tests/unit/rtu_stream_parser_test.cpp` | 13 项 parser 单元测试 | `79985edaf5a44afd1a11b955f0b388941d84bdccba97294c90f921abe322d2af` |

另修改根 `CMakeLists.txt` 和 `tests/CMakeLists.txt`，把 parser 加入 `gateway_core` 并注册 `gateway_rtu_stream_parser_test`。

## 6. Corpus seeds

| 文件 | 场景 | SHA256 |
|---|---|---|
| `bad_crc_then_valid.hex` | 坏 CRC 后合法请求 | `66a6f1d8d238e6cee03bf082fc17ca35cfbe5beb4b51cd2dcedfdfe5f57522e8` |
| `concatenated_requests.hex` | 两个粘连请求 | `f363f0638df0fe32d15f0dd672928b6aa6e5f501a0eb1150464c626ddf0f8165` |
| `exception_response.hex` | 合法异常响应 | `3b9df882ff6d517df11a5d2cc9e2d669237fa1c6414fed6e0154275f8f6240b6` |
| `noise_prefix.hex` | 两字节噪声前缀 | `f3d04bfc606f5bc288aab315ede5ca6c9bdf6158a65bb70d1943c77cd0782f82` |
| `oversized_response_prefix.hex` | 推导长度 259 的响应头后恢复 | `5f322e7c02309330cee42f2cff3e991b1fe39401cd7b9d0ed9f5800f72bfc9e9` |
| `truncated_response.hex` | 截断读取响应 | `a11b02116ed47cf8a4ed40b7ae2a924e6b4f75f72bca62149b8609ee9394356e` |
| `valid_request.hex` | 合法请求基线 | `cec4db6ffa9e5874a3b2d0f6ca5d594a7004357476a184e1a93a9654bbb6cef6` |

这些文件已由确定性单元测试回放，但不代表已经执行正式 fuzz campaign。

## 7. 质量门

所有配置均重新配置并执行 `--clean-first`：

| 配置 | 构建 | CTest | 结果边界 |
|---|---|---:|---|
| Debug + 警告视为错误 | 通过 | 33/33 | WSL 开发证据 |
| Debug + ASan/UBSan + 警告视为错误 | 通过 | 33/33 | 无 sanitizer 报告 |
| Release + 警告视为错误 | 通过 | 33/33 | 未提交工作区开发证据 |
| Debug + clang-tidy + 警告视为错误 | 通过 | 33/33 | 全量重建零警告 |

`clang-format --dry-run --Werror` 和 `git diff --check` 均通过。

## 8. 后续边界

T04 只生成内存中的请求、响应或结构化错误事件。`P3-S2-T05` 仍需完成：

- `openpty`/PTY 成对链路；
- 从站模拟器进程或工具目标；
- 从 PTY 读取字节并调用请求模式 parser；
- 根据寄存器表生成正常/异常、延迟、静默、坏 CRC 和截断响应；
- 真实文件描述符、超时和进程清理验证。

因此 T04 通过不表示 PTY、串口、RS485、寄存器业务或完整 G1/G2 已通过。
