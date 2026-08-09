# P3-S2-T03 Modbus RTU 编解码验收记录

## 1. 验收结论

- 任务：`P3-S2-T03`（`0x03`、`0x04`、`0x06` 编解码和异常响应）。
- 执行日期：2026-08-09。
- 代码基线：`7c8ffbd`（`main...origin/main`）。
- WSL 开发验收：**通过**。
- 干净提交 Release 复验：**待用户批准提交后执行**。

本任务已实现无状态的完整 RTU ADU codec，支持三个冻结功能码的请求、正常响应和异常响应，并在功能码解码前执行长度与 CRC 门控。协议非法输入通过固定结构的 `CodecError` 返回，不通过异常、无界容器、timeout 或重试策略表达。

执行开始前工作区已有 `src/protocol/crc16_modbus_原理说明.md` 的未提交修改。用户明确要求忽略该修改并直接执行 T03；本任务没有读取、覆盖、格式化或还原该文件，也不把它计入 T03 交付文件。

## 2. 合同与范围

独立仓库合同副本 `docs/protocol_subset.md` 的 SHA256 为：

```text
535385569a4452d93fd01dd0bdfae1c5158c73a530cd3081fdfb5d350508eb7c
```

实现范围：

- `0x03` Read Holding Registers 请求和响应；
- `0x04` Read Input Registers 请求和响应；
- `0x06` Write Single Register 请求和逐字节回显响应；
- `request_function | 0x80` 异常响应；
- CRC 生成、低字节先发和解码前校验；
- 从站、功能码、数量、地址溢出、长度、字节数及响应上下文匹配。

不在本任务范围：流式等待、半帧、噪声扫描、粘帧拆分、重新同步、PTY、串口、调度、重试，以及依据 `register_map.yaml` 执行寄存器存在性、可写性或工程量范围判断。

## 3. 接口与有界数据结构

codec 使用以下公开方向：

```text
Request  -> encode_request  -> Adu 或 CodecError
ADU      -> decode_request  -> Request 或 CodecError
Response -> encode_response -> Adu 或 CodecError
ADU + expected Request -> decode_response -> Response 或 CodecError
```

- ADU 使用固定 `256` 字节数组和显式长度；
- 读取响应使用固定 `125` 项寄存器数组和显式数量；
- `Request`、`Response` 和 `CodecResult` 使用 `std::variant`；
- 公共函数保持 `noexcept`；
- variant 访问使用 `std::get_if`，避免 `std::get/std::visit` 的理论异常逃逸路径。

## 4. 测试先行证据

先加入接口声明、16 条机器可读向量、13 项 codec 测试及 CMake 测试接线，但不加入实现。测试目标成功编译后在链接阶段按预期失败，核心诊断包括：

```text
undefined reference to `industrial_iot_gateway::protocol::encode_request(...)'
undefined reference to `industrial_iot_gateway::protocol::decode_request(...)'
undefined reference to `industrial_iot_gateway::protocol::encode_response(...)'
undefined reference to `industrial_iot_gateway::protocol::decode_response(...)'
collect2: error: ld returned 1 exit status
ninja: build stopped: subcommand failed.
```

加入最小实现后，同一构建目录转为 CTest 18/18；补齐 `0x04` 响应和编码/长度边界后转为最终 20/20。

首次 clang-tidy 发现 5 条 `bugprone-exception-escape` 警告，原因是 `noexcept` 函数内使用 `std::visit/std::get`。实现改为 `std::get_if` 和显式分支后，全量清理重建零警告，20/20 继续通过。

## 5. 交付文件

| 文件 | 用途 | SHA256 |
|---|---|---|
| `include/industrial_iot_gateway/protocol/modbus_codec.hpp` | 类型、错误类别和公开 API | `6420d6f79dee52a1fbcb98e16368e99a2a34013e034124d6b98c586771f837d9` |
| `src/protocol/modbus_codec.cpp` | ADU 编码、解码、CRC 门控和响应匹配 | `43b5283cd73a13e1a42d7b5d3388615bcac0102fcef4cadbe28ecab7eeed856f` |
| `tests/data/modbus_codec_vectors.csv` | 16 条 T03 机器可读协议向量 | `7b19461e4011ed1ce138d08dca783ed8462020b1360bef0b07f510d5bb80008d` |
| `tests/unit/modbus_codec_test.cpp` | 13 项 codec 单元测试 | `ad6921054bb81f21d02aaf634c543caacadcfef41316e6582ea8cb48af8eda55` |

另修改根 `CMakeLists.txt` 和 `tests/CMakeLists.txt`，把 codec 加入 `gateway_core` 并注册 `gateway_modbus_codec_test`。

## 6. 覆盖结果

测试覆盖：

1. `0x03`、`0x04`、`0x06` 冻结请求的精确编码与解码；
2. `0x03`、`0x04` 冻结正常响应及寄存器高字节先发；
3. `0x06` 正常响应精确回显及回显不一致拒绝；
4. 读取数量 `1..125` 和最大 125 寄存器、255 字节响应；
5. 数量 0、126 和 16 位地址范围溢出；
6. 广播地址 0、非法从站 248 和不支持功能码；
7. 短帧、超长帧、尾随字节和 CRC 错误；
8. 响应从站、功能码和 `byte_count` 不匹配；
9. 三条冻结异常向量、`0x01..0x04` 已知异常及未知异常码保留；
10. 空指针/零长度输入的有界拒绝。

## 7. 质量门结果

所有配置均执行 `--clean-first` 全量重建：

| 配置 | 构建 | CTest | 结果边界 |
|---|---|---:|---|
| Debug + 警告视为错误 | 通过 | 20/20 | WSL 开发证据 |
| Debug + ASan/UBSan + 警告视为错误 | 通过 | 20/20 | 无 sanitizer 报告 |
| Release + 警告视为错误 | 通过 | 20/20 | 脏工作区探索性证据 |
| Debug + clang-tidy + 警告视为错误 | 通过 | 20/20 | 全量重建零警告 |

`clang-format --dry-run --Werror` 和 `git diff --check` 均通过。

由于执行时工作区包含用户明确保留的 CRC 原理文档修改和 T03 未提交文件，本次 Release 结果不能替代冻结验收协议要求的干净工作区同一提交复验。

## 8. 后续边界

T03 的输入是调用方已经声明为完整候选 ADU 的连续字节。以下行为尚未实现，继续由 `P3-S2-T04` 负责：

- 一个帧被拆成多次读取时等待补帧；
- 多个粘连帧按顺序拆分；
- 噪声前缀扫描和有界重新同步；
- 坏 CRC、截断或超长流后的恢复；
- `t1.5`、`t3.5` 帧边界处理。

因此本结论只表示 codec 开发验收通过，不表示 parser、PTY、真实串口、寄存器映射执行或完整 G1/G2 已通过。
