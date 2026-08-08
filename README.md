# 工业物联网网关

`industrial_iot_gateway` 是一个采用 C++17 开发的工业通信网关项目。

## 当前范围

仓库目前只包含 `P3-S2-T01` 的构建、测试和质量门骨架。Modbus CRC、编解码器、流式解析器、PTY 集成、MQTT、systemd 和硬件支持均尚未实现。

## 构建与测试

```bash
cmake -S . -B build/debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_BUILD_TESTS=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

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
