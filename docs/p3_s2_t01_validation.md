# P3-S2-T01 验证记录

## 范围

本记录只覆盖 `P3-S2-T01` 创建的 C++17、CMake、CTest、GoogleTest、编译警告、sanitizer、格式检查和静态分析骨架。

本次验证前后仓库均没有提交。在项目所有者单独授权 Git 暂存和首次提交之前，所有项目文件保持未跟踪状态。

## 固化输入副本

已批准的 S1 规划基线已按字节原样复制到 `docs/repository_plan.md`。

```text
SHA256 a1219cfc b0719f24 876e8f08 07e16376 3e3db5d8 82a7f270 9f0fce7b 6ae1921e
```

以上空格仅用于显示分组。源文件和目标文件的哈希完全一致。

## 已创建目标

- `gateway_core`：只提供构建身份信息的最小静态库；
- `gateway_app`：链接 `gateway_core` 的最小可执行程序；
- `gateway_smoke_test`：注册到 CTest 的 GoogleTest 可执行程序，标签为 `unit`，超时为 10 s。

本任务没有实现 CRC、Modbus 编解码器、流式解析器、串口传输、PTY、MQTT、systemd 或硬件逻辑。

## 命令与结果

### Debug

```bash
cmake -S . -B build/debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_BUILD_TESTS=ON \
  -DGATEWAY_ENABLE_WARNINGS_AS_ERRORS=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

结果：配置生成 PASS，构建 PASS，CTest PASS 1/1。

### Sanitizer

```bash
cmake -S . -B build/sanitizer -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_BUILD_TESTS=ON \
  -DGATEWAY_ENABLE_WARNINGS_AS_ERRORS=ON \
  -DGATEWAY_ENABLE_SANITIZERS=ON
cmake --build build/sanitizer
ctest --test-dir build/sanitizer --output-on-failure
```

结果：配置生成 PASS，构建 PASS，CTest PASS 1/1，未出现 ASan/UBSan 报告。

### Release

```bash
cmake -S . -B build/release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGATEWAY_BUILD_TESTS=ON \
  -DGATEWAY_ENABLE_WARNINGS_AS_ERRORS=ON
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

结果：配置生成 PASS，构建 PASS，CTest PASS 1/1。

### 格式检查与静态分析

```bash
clang-format --dry-run --Werror \
  include/industrial_iot_gateway/build_info.hpp \
  src/gateway_core/build_info.cpp \
  src/app/main.cpp \
  tests/unit/build_info_test.cpp

cmake -S . -B build/clang-tidy -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_BUILD_TESTS=ON \
  -DGATEWAY_ENABLE_WARNINGS_AS_ERRORS=ON \
  -DGATEWAY_ENABLE_CLANG_TIDY=ON
cmake --build build/clang-tidy
ctest --test-dir build/clang-tidy --output-on-failure
```

结果：clang-format PASS；clang-tidy 配置生成和构建 PASS；CTest PASS 1/1。

## 证据边界

- 所有构建均在 x86_64 的 `Ubuntu-24.04-Gateway` 中原生执行，没有在 `/mnt/c` 或 `/mnt/f` 下构建。
- `build/` 已被忽略，不属于版本控制候选内容。
- ARM64 和树莓派验证均未执行。
- 项目许可证尚未最终确定，因此没有添加 `LICENSE` 文件。
- 本结果只足以通过 `P3-S2-T01` 骨架门，不满足后续 G1–G6 功能或 Release 验收门。
