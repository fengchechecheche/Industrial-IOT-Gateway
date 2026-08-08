# P3-S2-T02 CRC16 与协议向量验收记录

## 1. 验收结论

- 任务：`P3-S2-T02`（CRC16 与协议向量）。
- 执行日期：2026-08-08。
- 基线提交：`06fd09a`（`main`，远端跟踪分支为 `origin/main`）。
- 结论：**通过**。

本任务已实现独立的 CRC16/Modbus 数值计算 API，并将 S1 冻结的 Modbus RTU 帧转换为机器可读测试向量。测试覆盖标准参考值、空输入、单字节、最大 ADU 的 254 字节 CRC 输入、单比特扰动、冻结帧 CRC 以及线上低字节先发规则。

## 2. 合同基线

独立仓库维护副本：`docs/protocol_subset.md`。

- 源文档与维护副本 SHA256：`535385569a4452d93fd01dd0bdfae1c5158c73a530cd3081fdfb5d350508eb7c`。
- CRC 宽度：16 位。
- 初始值：`0xFFFF`。
- 反射多项式：`0xA001`。
- 最终异或：`0x0000`。
- CRC API 只返回数值，不负责线上字节序；RTU 帧按低字节、高字节发送。

## 3. 测试先行证据

在仅加入接口声明、测试和向量、尚未加入实现时，Debug 构建按预期失败。失败发生在 `gateway_crc16_test` 链接阶段，核心诊断为：

```text
undefined reference to `industrial_iot_gateway::protocol::crc16_modbus(unsigned char const*, unsigned long)'
collect2: error: ld returned 1 exit status
ninja: build stopped: subcommand failed.
```

测试源文件本身已成功编译，失败原因与待实现 CRC API 一致；加入最小实现和 CMake 源文件接线后，同一构建目录转为 7/7 测试通过。

## 4. 交付文件

| 文件 | 用途 | SHA256 |
|---|---|---|
| `include/industrial_iot_gateway/protocol/crc16_modbus.hpp` | CRC 常量与公开 API | `4b068a54b7779848f179d7a8cb5f02e74e63a4a7430b0a6de603116d3ff29739` |
| `src/protocol/crc16_modbus.cpp` | CRC16/Modbus 逐位实现 | `cbc5e596af81ab725bc97ae8316a9c9a06ede5f68516517a032b251ff5c6167d` |
| `tests/data/modbus_rtu_golden_vectors.csv` | 19 条冻结帧向量 | `995747dace392e334c9448b40071ffcee16d405d25ad731c84ece5569b6265f4` |
| `tests/unit/crc16_modbus_test.cpp` | 6 项 CRC 测试 | `d37fc7910f617a6ad091d3e280fc7052d45c6028e866b7f1610c5948c93162b0` |

另修改根 `CMakeLists.txt` 和 `tests/CMakeLists.txt`，将实现加入 `gateway_core`，并注册 `gateway_crc16_test`。

## 5. 验证结果

| 配置 | 构建 | CTest | 额外结果 |
|---|---|---:|---|
| Debug + 警告视为错误 | 通过 | 7/7 | 功能基线通过 |
| Debug + ASan/UBSan + 警告视为错误 | 通过 | 7/7 | 无 sanitizer 报告 |
| Release + 警告视为错误 | 通过 | 7/7 | 优化构建通过 |
| Debug + clang-tidy + 警告视为错误 | 通过 | 7/7 | 静态检查通过 |

`clang-format --dry-run --Werror` 和 `git diff --check` 均通过。

其中 CRC 测试包括：

1. 空输入返回初始值 `0xFFFF`；
2. 单字节 `00` 返回 `0x40BF`；
3. 标准校验串 `123456789` 返回 `0x4B37`；
4. 254 字节序列返回独立计算的参考值 `0x576C`；
5. 单比特翻转导致 CRC 改变；
6. 19 条冻结帧向量中，15 条 CRC 正确帧全部匹配，1 条坏 CRC 被识别，3 条不适用完整帧 CRC 校验的截断/噪声/粘帧样例被保留供后续 parser 使用。

## 6. 范围边界

本结论只验收 CRC16/Modbus 数值计算和协议向量基线，不表示以下功能已经完成：

- 功能码 `0x03`、`0x04`、`0x06` 编解码；
- Modbus 异常响应的结构化编解码；
- 增量流式组帧、坏帧恢复和重新同步；
- PTY 从站模拟器、真实串口、RS485 或 MQTT 集成。

上述内容分别属于 `P3-S2-T03` 及后续工作块。
