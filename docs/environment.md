# 开发环境

## 已验证环境

| 字段 | 内容 |
| ---- | ---- |
| 验证日期 | 2026-08-08 |
| WSL 发行版 | `Ubuntu-24.04-Gateway` |
| 操作系统 | Ubuntu 24.04.4 LTS |
| 架构 | x86_64 |
| 用户 | `iot-gw` |
| 仓库 | `/home/iot-gw/projects/industrial_iot_gateway` |
| CMake / CTest | 3.28.3 |
| GCC / G++ | 13.3.0 |
| Ninja | 1.11.1 |
| Clang | 18.1.3 |
| clang-format | 18.1.3 |
| clang-tidy | 18.1.3 |
| GoogleTest | 1.14.0 |
| yaml-cpp | 0.8.0 |
| glibc `openpty` / `libutil` | 2.39 |

## 安装来源

`P3-S2-T01` 工具链通过已配置的 Ubuntu 24.04 软件源安装，使用的命令为：

```bash
apt-get update
apt-get install -y \
  build-essential cmake ninja-build \
  clang clang-format clang-tidy \
  libgtest-dev pkg-config
```

`P3-S2-T05` 新增 YAML 解析开发依赖：

```bash
apt-get update
apt-get install -y --no-install-recommends libyaml-cpp-dev
```

PTY 链路直接使用 glibc 提供的 `<pty.h>`、`openpty()` 和 `libutil`。本任务没有安装
`socat`，也没有安装 MQTT、外部 Modbus 协议库或硬件专用依赖。

## P3-S2-T01 验证状态

| 配置 | 配置生成 | 构建 | CTest |
| ---- | -------- | ---- | ----- |
| Debug + warnings as errors | PASS | PASS | PASS，1/1 |
| ASan + UBSan | PASS | PASS | PASS，1/1 |
| Release | PASS | PASS | PASS，1/1 |
| clang-tidy | PASS | PASS | PASS，1/1 |

当前全部 C++ 源文件和头文件也通过了 `clang-format --dry-run --Werror`。

这些结果只验证构建、测试和质量门骨架，不验证 CRC、Modbus 编解码器、流式解析器、PTY 集成、MQTT、ARM64、systemd 或硬件行为。
